// Вариант 3 (v3_with_end_game): игра "у кого число больше" с завершением и победителем.
// Основа — вариант 2 (событийная рассылка, время UTC, keepalive).
//
// Правила:
//   - каждый игрок присылает число (можно передумать и прислать другое, пока игра не закончилась);
//   - как только числа есть от ДВУХ игроков — игра заканчивается:
//     у кого больше — победил, равные — ничья;
//   - сервер шлёт обоим SRV_GAME_END с результатом и удаляет их из списка игроков;
//   - клиенты, получив GAME_END, печатают результат и отключаются.
//
// Пока игра идёт, чужие числа НЕ показываются (только ready/waiting),
// иначе второй игрок мог бы подсмотреть число первого и ввести больше.
//
// Запуск:  ./v3_server [port]      (по умолчанию 7777)
// Формат пакетов описан в common/protocol.h

#include <sys/socket.h>   // socket, bind, recvfrom, sendto
#include <netinet/in.h>   // sockaddr_in, htons, INADDR_ANY
#include <arpa/inet.h>    // inet_ntop
#include <unistd.h>       // close
#include <poll.h>         // poll
#include <csignal>        // sigaction
#include <cerrno>         // errno, EINTR
#include <cstdio>         // printf, perror
#include <cstdlib>        // atoi
#include <cstring>        // memcpy
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>

#include "../common/protocol.h"

using Clock = std::chrono::steady_clock;

static volatile sig_atomic_t lastSignal = 0;

void onSignal(int sig) {
    lastSignal = sig;
}

struct Player {
    uint32_t id = 0;
    sockaddr_in addr{};              // куда слать этому игроку
    int32_t value = 0;               // присланное число
    bool submitted = false;          // прислал ли число (HELLO не считается)
    Clock::time_point lastSeen;      // когда последний раз что-то присылал (для таймаута)
};

using Players = std::unordered_map<uint64_t, Player>;

// Ключ игрока: IP (32 бита) + порт (16 бит) в одном числе
uint64_t makeKey(const sockaddr_in& a) {
    return ((uint64_t)a.sin_addr.s_addr << 16) | a.sin_port;
}

std::string addrToString(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

// --- Отправка сообщений клиентам ---

// [type][text...]
void sendState(int sock, const sockaddr_in& to, const std::string& text) {
    std::vector<uint8_t> buf;
    buf.push_back(SRV_STATE);
    buf.insert(buf.end(), text.begin(), text.end());
    sendto(sock, buf.data(), buf.size(), 0, (const sockaddr*)&to, sizeof(to));
}

// [type][result][text...]
void sendGameEnd(int sock, const sockaddr_in& to, GameResult result, const std::string& text) {
    std::vector<uint8_t> buf;
    buf.push_back(SRV_GAME_END);
    buf.push_back(result);
    buf.insert(buf.end(), text.begin(), text.end());

    // UDP может потерять пакет. GAME_END важен, поэтому шлём его 3 раза:
    // клиент обработает первый дошедший и выйдет, остальные просто никто не прочитает.
    for (int i = 0; i < 3; i++) {
        sendto(sock, buf.data(), buf.size(), 0, (const sockaddr*)&to, sizeof(to));
    }
}

// Текст состояния: кто готов, кто ещё думает. Сами числа не показываем!
std::string buildState(const Players& players, const std::string& event,
                       uint64_t clientSendUs, uint64_t serverRecvUs) {
    std::string s;
    for (const auto& [key, p] : players) {
        s += "P" + std::to_string(p.id) + "=" + (p.submitted ? "ready" : "waiting") + " ";
    }
    s += "| " + event;
    s += " | client sent: " + formatUtc(clientSendUs);
    s += " | server got: " + formatUtc(serverRecvUs);
    return s;
}

void broadcastState(int sock, const Players& players, const std::string& text) {
    for (const auto& [key, p] : players) {
        sendState(sock, p.addr, text);
    }
}

// --- Игровая логика ---

// Если числа прислали хотя бы двое — завершаем игру для этой пары.
void tryFinishGame(int sock, Players& players) {
    std::vector<uint64_t> ready;
    for (const auto& [key, p] : players) {
        if (p.submitted) ready.push_back(key);
    }
    if (ready.size() < 2) return;   // ждём второго

    Player a = players[ready[0]];
    Player b = players[ready[1]];

    std::string summary = "P" + std::to_string(a.id) + "=" + std::to_string(a.value) +
                          " vs P" + std::to_string(b.id) + "=" + std::to_string(b.value) + " → ";
    GameResult ra, rb;
    if (a.value > b.value) {
        ra = RESULT_WIN;  rb = RESULT_LOSE;
        summary += "P" + std::to_string(a.id) + " wins";
    } else if (b.value > a.value) {
        ra = RESULT_LOSE; rb = RESULT_WIN;
        summary += "P" + std::to_string(b.id) + " wins";
    } else {
        ra = RESULT_DRAW; rb = RESULT_DRAW;
        summary += "draw";
    }

    printf("[%s] GAME END: %s\n", formatUtc(nowUtcUs()).c_str(), summary.c_str());

    sendGameEnd(sock, a.addr, ra, summary);
    sendGameEnd(sock, b.addr, rb, summary);

    // Игроки этой партии больше не нужны: удаляем, места свободны для новой игры
    players.erase(ready[0]);
    players.erase(ready[1]);
}

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : DEFAULT_PORT;

    // 1. Сокет и порт
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // слушаем на всех сетевых интерфейсах

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    // 2. Корректная остановка по Ctrl+C
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Players players;
    uint32_t nextId = 1;

    // Клиент шлёт HELLO каждые 10 с. Если 30 с тишины — считаем, что он ушёл.
    const auto playerTimeout = std::chrono::seconds(30);

    printf("Server (v3, game with end) started on UDP port %u\n", port);

    while (lastSignal == 0) {
        // 3. Ждём пакет. Таймаут 1 с нужен только чтобы периодически
        //    проверять "молчащих" игроков, даже если пакетов нет.
        pollfd pfd{sock, POLLIN, 0};
        int ready = poll(&pfd, 1, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;   // Ctrl+C: while проверит lastSignal
            perror("poll");
            break;
        }

        // 4. Пришёл пакет
        if (ready > 0) {
            uint8_t buf[64];
            sockaddr_in client{};
            socklen_t clientLen = sizeof(client);
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&client, &clientLen);
            uint64_t recvUs = nowUtcUs();   // время получения по UTC — сразу после recvfrom

            ClientMsg msg;
            if (n < 0 || !decodeClientMsg(buf, (size_t)n, msg)) {
                if (n >= 0) printf("Bad packet from %s: %zd bytes\n", addrToString(client).c_str(), n);
                continue;
            }

            // найти игрока или создать нового
            auto [it, isNew] = players.try_emplace(makeKey(client));
            Player& p = it->second;
            p.lastSeen = Clock::now();

            if (isNew) {
                p.id = nextId++;
                p.addr = client;
                printf("[%s] P%u joined from %s\n",
                       formatUtc(recvUs).c_str(), p.id, addrToString(client).c_str());
                std::string event = "P" + std::to_string(p.id) + " joined";
                broadcastState(sock, players, buildState(players, event, msg.sendTimeUs, recvUs));
            }

            if (msg.type == MSG_VALUE) {
                p.value = msg.value;
                p.submitted = true;
                printf("[%s] P%u sent %d (client sent at %s)\n",
                       formatUtc(recvUs).c_str(), p.id, p.value, formatUtc(msg.sendTimeUs).c_str());

                // 5. СРАЗУ рассылаем новое состояние всем (включая отправителя)
                std::string event = "P" + std::to_string(p.id) + " is ready";
                broadcastState(sock, players, buildState(players, event, msg.sendTimeUs, recvUs));

                // 6. Если теперь готовы двое — завершаем игру
                tryFinishGame(sock, players);
            }
            // MSG_HELLO от уже известного игрока — просто keepalive, lastSeen обновили выше
        }

        // 7. Удаляем тех, кто давно молчит (в UDP клиент может исчезнуть без предупреждения)
        auto now = Clock::now();
        for (auto it = players.begin(); it != players.end(); ) {
            if (now - it->second.lastSeen > playerTimeout) {
                printf("[%s] P%u timed out\n", formatUtc(nowUtcUs()).c_str(), it->second.id);
                it = players.erase(it);
            } else {
                ++it;
            }
        }
    }

    printf("Shutting down\n");
    close(sock);
    return 0;
}
