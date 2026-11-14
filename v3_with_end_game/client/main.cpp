// Вариант 3 (v3_with_end_game): клиент игры "у кого число больше".
// Получив от сервера GAME_END, печатает победителя и отключается.
// Как и в варианте 2, клиент одновременно
//   - ждёт ввод числа с клавиатуры и отправляет его серверу,
//   - и в любой момент принимает обновления состояния от сервера
//     (в том числе когда число прислал ДРУГОЙ игрок),
//   - раз в 10 с шлёт серверу HELLO (keepalive), чтобы сервер не удалил нас,
//     а роутер (NAT) не "забыл" наш адрес.
//
// Запуск:  ./v3_client [host] [port]     (по умолчанию 127.0.0.1 7777)
//   host — IP-адрес или имя (например, my-server.example.com)
// Формат пакетов описан в common/protocol.h

#include <sys/socket.h>   // socket, sendto, recvfrom
#include <netinet/in.h>   // sockaddr_in
#include <arpa/inet.h>
#include <netdb.h>        // getaddrinfo — превращает имя/IP в адрес
#include <unistd.h>       // close, STDIN_FILENO
#include <poll.h>         // poll
#include <cstdio>         // printf, fgets, perror
#include <cstring>        // strcspn, memcpy
#include <cstdint>
#include <charconv>       // std::from_chars
#include <chrono>
#include <string>

#include "../common/protocol.h"

using Clock = std::chrono::steady_clock;

void sendMsg(int sock, const sockaddr_in& server, MsgType type, int32_t value) {
    ClientMsg m;
    m.type = type;
    m.value = value;
    m.sendTimeUs = nowUtcUs();   // время отправки по UTC — прямо перед sendto

    uint8_t buf[CLIENT_MSG_SIZE];
    encodeClientMsg(m, buf);
    if (sendto(sock, buf, sizeof(buf), 0, (const sockaddr*)&server, sizeof(server)) < 0) {
        perror("sendto");
    }
}

// "1.2.3.4" или "my-server.example.com" → sockaddr_in
bool resolve(const char* host, uint16_t port, sockaddr_in& out) {
    addrinfo hints{};
    hints.ai_family = AF_INET;        // только IPv4
    hints.ai_socktype = SOCK_DGRAM;   // UDP

    addrinfo* res = nullptr;
    int err = getaddrinfo(host, nullptr, &hints, &res);
    if (err != 0) {
        printf("Cannot resolve %s: %s\n", host, gai_strerror(err));
        return false;
    }
    memcpy(&out, res->ai_addr, sizeof(out));
    out.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : DEFAULT_PORT;

    // 1. Сокет (bind не нужен: порт выдаст ОС)
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // 2. Адрес сервера
    sockaddr_in server{};
    if (!resolve(host, port, server)) {
        return 1;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &server.sin_addr, ip, sizeof(ip));
    printf("Server: %s:%u\n", ip, port);

    // 3. Сразу "представляемся", чтобы сервер запомнил наш адрес
    sendMsg(sock, server, MSG_HELLO, 0);

    const auto keepaliveInterval = std::chrono::seconds(10);
    auto nextKeepalive = Clock::now() + keepaliveInterval;

    printf("Type a number and press Enter (Ctrl+D to quit)\n");

    // 4. Следим сразу за двумя источниками событий:
    //    fds[0] — клавиатура, fds[1] — сокет
    pollfd fds[2];
    fds[0] = {STDIN_FILENO, POLLIN, 0};
    fds[1] = {sock, POLLIN, 0};

    while (true) {
        // спим, пока не произойдёт событие или не придёт время keepalive
        int timeoutMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                            nextKeepalive - Clock::now()).count();
        if (timeoutMs < 0) timeoutMs = 0;

        if (poll(fds, 2, timeoutMs) < 0) {
            perror("poll");
            break;
        }

        // 5. Пора отправить keepalive
        if (Clock::now() >= nextKeepalive) {
            sendMsg(sock, server, MSG_HELLO, 0);
            nextKeepalive = Clock::now() + keepaliveInterval;
        }

        // 6. Пользователь ввёл строку
        if (fds[0].revents & POLLIN) {
            char line[256];
            if (!fgets(line, sizeof(line), stdin)) {
                break;   // Ctrl+D — выходим
            }
            size_t len = strcspn(line, "\n");

            int32_t value;
            auto [ptr, ec] = std::from_chars(line, line + len, value);
            if (ec == std::errc::result_out_of_range) {
                printf("Number is too big for int32\n");
            } else if (ec != std::errc() || ptr != line + len) {
                printf("Not a number\n");
            } else {
                sendMsg(sock, server, MSG_VALUE, value);
            }
        }

        // 7. Сервер прислал сообщение (от нашего действия или от другого игрока)
        if (fds[1].revents & POLLIN) {
            uint8_t buf[1024];
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
            uint64_t recvUs = nowUtcUs();   // когда МЫ получили сообщение
            if (n < 1) continue;

            // первый байт — тип сообщения, по нему решаем, как читать остальное
            if (buf[0] == SRV_STATE) {
                std::string text((const char*)buf + 1, (size_t)n - 1);
                printf("[update] %s | I got: %s\n", text.c_str(), formatUtc(recvUs).c_str());

            } else if (buf[0] == SRV_GAME_END && n >= 2) {
                uint8_t result = buf[1];
                std::string text((const char*)buf + 2, (size_t)n - 2);

                printf("\n=== GAME OVER: %s ===\n", text.c_str());
                if (result == RESULT_WIN)       printf("You WIN!\n");
                else if (result == RESULT_LOSE) printf("You lose.\n");
                else                            printf("Draw.\n");

                // сервер сказал, что игра окончена — завершаем работу
                close(sock);
                return 0;
            }
        }
    }

    close(sock);
    return 0;
}
