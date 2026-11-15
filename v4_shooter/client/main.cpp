// Вариант 4 (v4_shooter): клиент стрелялки с окном (raylib).
//
// Управление: WASD — движение, мышь — прицел, левая кнопка мыши или пробел — выстрел.
//
// Каждый кадр (60 раз в секунду) клиент:
//   1. забирает ВСЕ пришедшие от сервера пакеты (сокет неблокирующий) и оставляет самое свежее состояние;
//   2. читает клавиатуру и мышь и отправляет серверу INPUT (только ввод, НЕ координаты);
//   3. рисует мир таким, каким его прислал сервер.
//
// Клиент ничего не решает сам: где игроки, где пули, кто попал — всё приходит с сервера.
//
// Запуск:  ./v4_client [host] [port]     (по умолчанию 127.0.0.1 7777)
// Нужна библиотека raylib:  brew install raylib

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>        // fcntl — сделать сокет неблокирующим
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "raylib.h"
#include "../common/protocol.h"

// Окно: сверху полоса с информацией (HUD), под ней карта 1:1
constexpr int HUD_HEIGHT = 40;
constexpr int WINDOW_W = (int)MAP_SIZE;
constexpr int WINDOW_H = (int)MAP_SIZE + HUD_HEIGHT;

bool resolve(const char* host, uint16_t port, sockaddr_in& out) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
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

void sendPacket(int sock, const sockaddr_in& server, const Writer& w) {
    sendto(sock, w.data(), w.size(), 0, (const sockaddr*)&server, sizeof(server));
}

// Текст по центру экрана
void drawCentered(const char* text, int y, int fontSize, Color color) {
    int width = MeasureText(text, fontSize);
    DrawText(text, (WINDOW_W - width) / 2, y, fontSize, color);
}

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : DEFAULT_PORT;

    // ---------- Сеть ----------

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    // Неблокирующий сокет: recvfrom сразу возвращает -1 (EAGAIN), если пакетов нет.
    // Игра не должна "замирать" в ожидании сети — кадры рисуются всегда.
    fcntl(sock, F_SETFL, O_NONBLOCK);

    sockaddr_in server{};
    if (!resolve(host, port, server)) return 1;

    // ---------- Окно ----------

    SetTraceLogLevel(LOG_WARNING);   // меньше служебного вывода raylib в консоль
    InitWindow(WINDOW_W, WINDOW_H, "Online Game v4");
    SetTargetFPS(60);

    // ---------- Состояние клиента ----------

    uint8_t myId = 0;                // 0 — сервер ещё не прислал WELCOME
    bool serverFull = false;

    StateMsg state;                  // последнее состояние мира от сервера
    bool haveState = false;

    GameEndMsg gameEnd;
    bool gameOver = false;

    uint32_t inputSeq = 0;
    double lastHelloAt = -10;
    double lastPacketAt = GetTime();

    while (!WindowShouldClose()) {
        double now = GetTime();

        // ---------- 1. Приём: забираем все пакеты, что пришли ----------
        while (true) {
            uint8_t buf[2048];
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0) break;   // пакетов больше нет
            lastPacketAt = now;

            Reader r(buf, (size_t)n);
            uint8_t type = r.u8();

            if (type == SRV_WELCOME) {
                myId = r.u8();
            } else if (type == SRV_FULL) {
                serverFull = true;
            } else if (type == SRV_STATE) {
                StateMsg s;
                // пакеты могут прийти не по порядку: берём только более новый тик
                if (decodeState(r, s) && (!haveState || s.tick > state.tick)) {
                    state = s;
                    haveState = true;
                }
            } else if (type == SRV_GAME_END && !gameOver) {
                if (decodeGameEnd(r, gameEnd)) gameOver = true;
            }
        }

        // Ищем себя в состоянии (нужно для прицела)
        const PlayerState* me = nullptr;
        if (haveState) {
            for (const auto& p : state.players) if (p.id == myId) me = &p;
        }

        // ---------- 2. Отправка ----------
        if (!gameOver && !serverFull) {
            if (myId == 0) {
                // ещё не в игре — стучимся, пока сервер не ответит WELCOME
                if (now - lastHelloAt > 0.5) {
                    sendPacket(sock, server, encodeHello());
                    lastHelloAt = now;
                }
            } else {
                InputMsg in;
                in.seq = ++inputSeq;
                if (IsKeyDown(KEY_W)) in.keys |= IN_UP;
                if (IsKeyDown(KEY_S)) in.keys |= IN_DOWN;
                if (IsKeyDown(KEY_A)) in.keys |= IN_LEFT;
                if (IsKeyDown(KEY_D)) in.keys |= IN_RIGHT;
                if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsKeyDown(KEY_SPACE)) in.keys |= IN_FIRE;

                // угол от своего игрока к курсору мыши (в координатах карты)
                if (me) {
                    Vector2 mouse = GetMousePosition();
                    in.aim = std::atan2((mouse.y - HUD_HEIGHT) - me->y, mouse.x - me->x);
                }
                sendPacket(sock, server, encodeInput(in));
            }
        }

        // ---------- 3. Рисование ----------
        BeginDrawing();
        ClearBackground(RAYWHITE);

        // Карта
        DrawRectangle(0, HUD_HEIGHT, WINDOW_W, (int)MAP_SIZE, Color{235, 235, 235, 255});
        DrawRectangleLines(0, HUD_HEIGHT, WINDOW_W, (int)MAP_SIZE, DARKGRAY);

        if (haveState) {
            // Пули: свои синие, чужие красные
            for (const auto& b : state.bullets) {
                Color c = (b.owner == myId) ? DARKBLUE : MAROON;
                DrawCircleV(Vector2{b.x, b.y + HUD_HEIGHT}, BULLET_RADIUS, c);
            }

            // Игроки
            for (const auto& p : state.players) {
                Vector2 pos{p.x, p.y + HUD_HEIGHT};
                Color body = !p.alive ? LIGHTGRAY : (p.id == myId ? BLUE : RED);

                // линия прицела
                Vector2 tip{pos.x + std::cos(p.aim) * (PLAYER_RADIUS + 14),
                            pos.y + std::sin(p.aim) * (PLAYER_RADIUS + 14)};
                DrawLineEx(pos, tip, 4, DARKGRAY);

                DrawCircleV(pos, PLAYER_RADIUS, body);
                DrawText(p.id == myId ? "you" : TextFormat("P%d", p.id),
                         (int)pos.x - 12, (int)pos.y - (int)PLAYER_RADIUS - 18, 14, DARKGRAY);
            }
        }

        // HUD сверху
        DrawRectangle(0, 0, WINDOW_W, HUD_HEIGHT, Color{40, 40, 40, 255});
        if (haveState) {
            int myScore = 0, enemyScore = 0;
            for (const auto& p : state.players) {
                if (p.id == myId) myScore = p.score; else enemyScore = p.score;
            }
            DrawText(TextFormat("Round %d/%d", state.round, state.roundsTotal), 12, 10, 20, RAYWHITE);
            DrawText(TextFormat("You %d : %d Enemy", myScore, enemyScore), WINDOW_W / 2 - 80, 10, 20, RAYWHITE);
        }
        DrawText(TextFormat("%s:%d", host, port), WINDOW_W - 200, 12, 16, GRAY);

        // Надписи поверх карты в зависимости от фазы
        int midY = HUD_HEIGHT + (int)MAP_SIZE / 2 - 60;
        if (gameOver) {
            const char* title = gameEnd.winnerId == 0      ? "DRAW"
                              : gameEnd.winnerId == myId   ? "YOU WIN!"
                                                           : "YOU LOSE";
            Color c = gameEnd.winnerId == myId ? DARKGREEN : (gameEnd.winnerId == 0 ? DARKGRAY : MAROON);
            drawCentered(title, midY, 60, c);

            int myScore = 0, enemyScore = 0;
            for (const auto& s : gameEnd.scores) {
                if (s.id == myId) myScore = s.score; else enemyScore = s.score;
            }
            drawCentered(TextFormat("Final score  %d : %d", myScore, enemyScore), midY + 80, 30, DARKGRAY);
            drawCentered("Close the window to exit", midY + 130, 20, GRAY);
        } else if (serverFull) {
            drawCentered("Server is full", midY, 40, MAROON);
        } else if (myId == 0) {
            drawCentered(TextFormat("Connecting to %s:%d ...", host, port), midY, 30, DARKGRAY);
        } else if (now - lastPacketAt > 3.0) {
            drawCentered("Connection lost...", midY, 40, MAROON);
        } else if (haveState) {
            if (state.phase == PHASE_WAITING) {
                drawCentered("Waiting for opponent...", midY, 30, DARKGRAY);
            } else if (state.phase == PHASE_COUNTDOWN) {
                drawCentered(TextFormat("Round %d", state.round), midY, 50, DARKGRAY);
                drawCentered(TextFormat("%.1f", state.timeLeftMs / 1000.0f), midY + 60, 30, GRAY);
            } else if (state.phase == PHASE_ROUND_OVER) {
                const char* text = state.lastScorer == 0    ? "Both hit!"
                                 : state.lastScorer == myId ? "You hit!"
                                                            : "You were hit!";
                drawCentered(text, midY, 50, state.lastScorer == myId ? DARKGREEN : MAROON);
            }
        }

        EndDrawing();
    }

    // Закрыли окно посреди игры — предупреждаем сервер (если не дойдёт, сработает таймаут 5 с)
    if (myId != 0 && !gameOver) sendPacket(sock, server, encodeLeave());

    CloseWindow();
    close(sock);
    return 0;
}
