// Вариант 5 (v5_prediction): клиент с client-side prediction и интерполяцией.
//
// Управление: WASD — движение, мышь — прицел, ЛКМ или пробел — выстрел.
// Для экспериментов:  P — вкл/выкл предсказание,  I — вкл/выкл интерполяцию соперника.
//
// Три техники, которые здесь показаны:
//
// 1. PREDICTION (предсказание своего движения).
//    Нажал клавишу → клиент СРАЗУ двигает себя функцией applyMovement() (та же, что на сервере)
//    и отправляет INPUT. Задержки управления нет.
//
// 2. RECONCILIATION (сверка с сервером).
//    Сервер присылает свою позицию игрока и lastInputSeq — какой ввод он учёл последним.
//    Клиент берёт позицию сервера как истину и заново применяет к ней свои вводы,
//    которые сервер ещё не учёл. Если всё совпало — игрок ничего не заметит.
//    Если нет (сервер сдвинул на спавн, потерялся пакет) — позиция поправится.
//
// 3. INTERPOLATION (плавный соперник).
//    Соперника рисуем чуть "в прошлом" — между двумя полученными снимками мира.
//    Какой момент рисовать, решает RenderClock (render_clock.h): он идёт ровно
//    с реальным временем и сам подбирает запас под неровность сети.
//
// Запуск:  ./v5_client [host] [port]     (по умолчанию 127.0.0.1 7777)
// Нужна библиотека raylib:  brew install raylib

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <vector>

#include "raylib.h"
#include "../common/protocol.h"
#include "render_clock.h"

constexpr int HUD_HEIGHT = 40;
constexpr int WINDOW_W = (int)MAP_SIZE;
constexpr int WINDOW_H = (int)MAP_SIZE + HUD_HEIGHT;

// Ввод, отправленный серверу, но ещё не подтверждённый им
struct PendingInput {
    InputMsg in;
    double sentAt;   // когда отправили — для оценки пинга
};

// Снимок мира от сервера (для интерполяции)
struct Snapshot {
    uint32_t tick;
    std::vector<PlayerState> players;
    std::vector<BulletState> bullets;
};

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

void drawCentered(const char* text, int y, int fontSize, Color color) {
    int width = MeasureText(text, fontSize);
    DrawText(text, (WINDOW_W - width) / 2, y, fontSize, color);
}

const PlayerState* findPlayer(const std::vector<PlayerState>& players, uint8_t id) {
    for (const auto& p : players) if (p.id == id) return &p;
    return nullptr;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Плавный переход угла (через кратчайшую сторону, чтобы не крутиться на 350° вместо 10°)
float lerpAngle(float a, float b, float t) {
    float d = std::remainder(b - a, 2.0f * (float)M_PI);
    return a + d * t;
}

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : DEFAULT_PORT;

    // ---------- Сеть ----------
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    fcntl(sock, F_SETFL, O_NONBLOCK);

    sockaddr_in server{};
    if (!resolve(host, port, server)) return 1;

    // ---------- Окно ----------
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(WINDOW_W, WINDOW_H, "Online Game v5 (prediction)");
    SetTargetFPS(60);

    // ---------- Состояние клиента ----------
    uint8_t myId = 0;
    bool serverFull = false;

    StateMsg latest;                 // самое свежее состояние (фаза, счёт, кто жив)
    bool haveState = false;
    double latestAt = 0;             // когда (по нашим часам) пришло latest — для полёта своих пуль

    std::deque<Snapshot> snapshots;  // история для интерполяции (~1 с)
    RenderClock renderClock;         // какой момент сервера рисовать для соперника

    GameEndMsg gameEnd;
    bool gameOver = false;

    // предсказание
    bool predictionOn = true;
    bool interpolationOn = true;
    float predX = 0, predY = 0;      // где мы, по нашему предсказанию
    bool havePrediction = false;
    std::deque<PendingInput> pending;
    uint32_t inputSeq = 0;
    double inputAccumulator = 0;
    float lastCorrection = 0;        // насколько сервер поправил предсказание (для отладки)

    double rttMs = 0;                // оценка пинга
    double lastHelloAt = -10;
    double lastPacketAt = GetTime();

    while (!WindowShouldClose()) {
        double now = GetTime();

        if (IsKeyPressed(KEY_P)) predictionOn = !predictionOn;
        if (IsKeyPressed(KEY_I)) interpolationOn = !interpolationOn;

        // ---------- 1. Приём ----------
        while (true) {
            uint8_t buf[2048];
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0) break;
            lastPacketAt = now;

            Reader r(buf, (size_t)n);
            uint8_t type = r.u8();

            if (type == SRV_WELCOME) {
                myId = r.u8();
            } else if (type == SRV_FULL) {
                serverFull = true;
            } else if (type == SRV_GAME_END && !gameOver) {
                if (decodeGameEnd(r, gameEnd)) gameOver = true;
            } else if (type == SRV_STATE) {
                StateMsg s;
                if (!decodeState(r, s)) continue;
                if (haveState && s.tick <= latest.tick) continue;   // устаревший пакет
                latest = s;
                haveState = true;
                latestAt = now;
                renderClock.onSnapshot(s.tick, now, TICK_RATE);

                snapshots.push_back({s.tick, s.players, s.bullets});
                while (snapshots.size() > 60) snapshots.pop_front();

                // ---- RECONCILIATION ----
                const PlayerState* me = findPlayer(s.players, myId);
                if (me) {
                    // пинг: сколько прошло с отправки ввода, который сервер только что подтвердил
                    for (const auto& p : pending) {
                        if (p.in.seq == me->lastInputSeq) {
                            double sample = (now - p.sentAt) * 1000.0;
                            rttMs = rttMs == 0 ? sample : rttMs * 0.9 + sample * 0.1;
                            break;
                        }
                    }
                    // выбрасываем вводы, которые сервер уже учёл
                    while (!pending.empty() && pending.front().in.seq <= me->lastInputSeq) {
                        pending.pop_front();
                    }
                    // берём позицию сервера и заново применяем свои неподтверждённые вводы
                    float x = me->x, y = me->y;
                    for (const auto& p : pending) {
                        if (canMove(s.phase, me->alive)) applyMovement(x, y, p.in.keys, INPUT_DT);
                    }
                    if (havePrediction) lastCorrection = std::hypot(x - predX, y - predY);
                    predX = x;
                    predY = y;
                    havePrediction = true;
                }
            }
        }

        const PlayerState* meServer = haveState ? findPlayer(latest.players, myId) : nullptr;

        // Где рисовать себя: предсказание или (если выключено) то, что прислал сервер
        float myX = meServer ? meServer->x : 0, myY = meServer ? meServer->y : 0;
        if (predictionOn && havePrediction) { myX = predX; myY = predY; }

        // Прицел — от своей позиции к мыши (считаем сразу, без ожидания сервера)
        Vector2 mouse = GetMousePosition();
        float myAim = std::atan2((mouse.y - HUD_HEIGHT) - myY, mouse.x - myX);

        // ---------- 2. Отправка: ровно 60 вводов в секунду, независимо от FPS ----------
        if (!gameOver && !serverFull) {
            if (myId == 0) {
                if (now - lastHelloAt > 0.5) {
                    sendPacket(sock, server, encodeHello());
                    lastHelloAt = now;
                }
            } else {
                inputAccumulator += GetFrameTime();
                if (inputAccumulator > 0.25) inputAccumulator = 0.25;   // после долгой паузы не "отстреливаем" сотню вводов

                while (inputAccumulator >= INPUT_DT) {
                    inputAccumulator -= INPUT_DT;

                    InputMsg in;
                    in.seq = ++inputSeq;
                    if (IsKeyDown(KEY_W)) in.keys |= IN_UP;
                    if (IsKeyDown(KEY_S)) in.keys |= IN_DOWN;
                    if (IsKeyDown(KEY_A)) in.keys |= IN_LEFT;
                    if (IsKeyDown(KEY_D)) in.keys |= IN_RIGHT;
                    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsKeyDown(KEY_SPACE)) in.keys |= IN_FIRE;
                    in.aim = myAim;

                    // ---- PREDICTION: двигаем себя сразу, той же функцией, что и сервер ----
                    if (havePrediction && meServer && canMove(latest.phase, meServer->alive)) {
                        applyMovement(predX, predY, in.keys, INPUT_DT);
                    }

                    pending.push_back({in, now});
                    while (pending.size() > 120) pending.pop_front();   // сервер давно молчит — не копим бесконечно
                    // шлём этот ввод + несколько предыдущих (на случай потери пакетов)
                    InputMsg recent[INPUT_REDUNDANCY];
                    int count = 0;
                    size_t first = pending.size() > (size_t)INPUT_REDUNDANCY ? pending.size() - INPUT_REDUNDANCY : 0;
                    for (size_t i = first; i < pending.size(); i++) recent[count++] = pending[i].in;
                    sendPacket(sock, server, encodeInput(recent, count));
                }
            }
        }

        // Пересчитываем позицию после ввода этого кадра
        if (predictionOn && havePrediction) { myX = predX; myY = predY; }

        // ---------- 3. INTERPOLATION: находим два снимка вокруг "времени отрисовки" ----------
        renderClock.advance(now, GetFrameTime(), TICK_RATE);
        double renderTick = renderClock.renderTick;
        const Snapshot* from = nullptr;
        const Snapshot* to = nullptr;
        float t = 0;
        if (!snapshots.empty()) {
            if (renderTick >= snapshots.back().tick) {
                // свежее снимков нет (пакет опоздал сильнее запаса) — стоим на последнем,
                // а не прыгаем куда-то: часы сами чуть замедлятся
                from = to = &snapshots.back();
            } else if (renderTick < snapshots.front().tick) {
                from = to = &snapshots.front();
            } else {
                for (size_t i = 0; i + 1 < snapshots.size(); i++) {
                    if (snapshots[i].tick <= renderTick && renderTick < snapshots[i + 1].tick) {
                        from = &snapshots[i];
                        to = &snapshots[i + 1];
                        t = (float)((renderTick - from->tick) / (double)(to->tick - from->tick));
                        break;
                    }
                }
            }
        }

        // ---------- 4. Рисование ----------
        BeginDrawing();
        ClearBackground(RAYWHITE);
        DrawRectangle(0, HUD_HEIGHT, WINDOW_W, (int)MAP_SIZE, Color{235, 235, 235, 255});
        DrawRectangleLines(0, HUD_HEIGHT, WINDOW_W, (int)MAP_SIZE, DARKGRAY);

        if (haveState) {
            // Пули: свои — из самого свежего состояния (меньше задержка после выстрела),
            // чужие — из того же момента, в котором рисуем соперника (чтобы попадания выглядели честно)
            // Свою пулю досчитываем сами: позиция из пакета + скорость × время с его прихода.
            // Иначе при залипании сети пуля висит в воздухе, а потом прыгает.
            float flight = (float)std::min(now - latestAt, 0.3);
            for (const auto& b : latest.bullets) {
                if (b.owner != myId) continue;
                float bx = b.x + b.vx * flight, by = b.y + b.vy * flight;
                if (bx < 0 || by < 0 || bx > MAP_SIZE || by > MAP_SIZE) continue;   // уже улетела за стену
                DrawCircleV(Vector2{bx, by + HUD_HEIGHT}, BULLET_RADIUS, DARKBLUE);
            }
            const std::vector<BulletState>& enemyBullets =
                (interpolationOn && from) ? from->bullets : latest.bullets;
            for (const auto& b : enemyBullets) {
                if (b.owner != myId) DrawCircleV(Vector2{b.x, b.y + HUD_HEIGHT}, BULLET_RADIUS, MAROON);
            }

            for (const auto& p : latest.players) {
                float x = p.x, y = p.y, aim = p.aim;

                if (p.id == myId) {
                    x = myX; y = myY; aim = myAim;   // себя — по предсказанию, прицел мгновенный
                } else if (interpolationOn && from && to) {
                    const PlayerState* a = findPlayer(from->players, p.id);
                    const PlayerState* b = findPlayer(to->players, p.id);
                    if (a && b) {
                        x = lerp(a->x, b->x, t);
                        y = lerp(a->y, b->y, t);
                        aim = lerpAngle(a->aim, b->aim, t);
                    }
                }

                Vector2 pos{x, y + HUD_HEIGHT};
                Color body = !p.alive ? LIGHTGRAY : (p.id == myId ? BLUE : RED);
                Vector2 tip{pos.x + std::cos(aim) * (PLAYER_RADIUS + 14),
                            pos.y + std::sin(aim) * (PLAYER_RADIUS + 14)};
                DrawLineEx(pos, tip, 4, DARKGRAY);
                DrawCircleV(pos, PLAYER_RADIUS, body);
                DrawText(p.id == myId ? "you" : TextFormat("P%d", p.id),
                         (int)pos.x - 12, (int)pos.y - (int)PLAYER_RADIUS - 18, 14, DARKGRAY);

                // Для наглядности: где себя видит СЕРВЕР (контур). С prediction он чуть отстаёт.
                if (p.id == myId && predictionOn) {
                    DrawCircleLines((int)p.x, (int)(p.y + HUD_HEIGHT), PLAYER_RADIUS, Fade(DARKBLUE, 0.4f));
                }
            }
        }

        // HUD
        DrawRectangle(0, 0, WINDOW_W, HUD_HEIGHT, Color{40, 40, 40, 255});
        if (haveState) {
            int myScore = 0, enemyScore = 0;
            for (const auto& p : latest.players) {
                if (p.id == myId) myScore = p.score; else enemyScore = p.score;
            }
            DrawText(TextFormat("Round %d/%d", latest.round, latest.roundsTotal), 12, 10, 20, RAYWHITE);
            DrawText(TextFormat("You %d : %d Enemy", myScore, enemyScore), 190, 10, 20, RAYWHITE);
        }
        // ping — от отправки ввода до его применения сервером (сеть туда-обратно + ожидание в очереди сервера)
        // interp — насколько в прошлом рисуем соперника (растёт, если сеть неровная)
        DrawText(TextFormat("ping %.0f | interp %.0f ms | [P]red %s | [I]nterp %s | fix %.1f",
                            rttMs, renderClock.delayTicks * 1000.0 / TICK_RATE,
                            predictionOn ? "ON" : "off", interpolationOn ? "ON" : "off", lastCorrection),
                 390, 13, 14, GRAY);

        int midY = HUD_HEIGHT + (int)MAP_SIZE / 2 - 60;
        if (gameOver) {
            const char* title = gameEnd.winnerId == 0    ? "DRAW"
                              : gameEnd.winnerId == myId ? "YOU WIN!"
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
            if (latest.phase == PHASE_WAITING) {
                drawCentered("Waiting for opponent...", midY, 30, DARKGRAY);
            } else if (latest.phase == PHASE_COUNTDOWN) {
                drawCentered(TextFormat("Round %d", latest.round), midY, 50, DARKGRAY);
                drawCentered(TextFormat("%.1f", latest.timeLeftMs / 1000.0f), midY + 60, 30, GRAY);
            } else if (latest.phase == PHASE_ROUND_OVER) {
                const char* text = latest.lastScorer == 0    ? "Both hit!"
                                 : latest.lastScorer == myId ? "You hit!"
                                                             : "You were hit!";
                drawCentered(text, midY, 50, latest.lastScorer == myId ? DARKGREEN : MAROON);
            }
        }

        EndDrawing();
    }

    if (myId != 0 && !gameOver) sendPacket(sock, server, encodeLeave());
    CloseWindow();
    close(sock);
    return 0;
}
