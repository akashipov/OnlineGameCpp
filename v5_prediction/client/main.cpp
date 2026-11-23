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
// Нужна библиотека raylib:  brew install raylib  (Windows: см. README_WINDOWS.md)
// Работает на Mac, Linux и Windows: всё, что отличается между системами (сокеты), — в net.cpp.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <deque>
#include <random>
#include <vector>

#include "raylib.h"
#include "../common/protocol.h"
#include "render_clock.h"
#include "net.h"

constexpr int HUD_HEIGHT = 40;
constexpr int WINDOW_W = (int)MAP_SIZE;
constexpr int WINDOW_H = (int)MAP_SIZE + HUD_HEIGHT;

// Ввод, отправленный серверу, но ещё не подтверждённый им
struct PendingInput {
    InputMsg in;
    double sentAt;   // когда отправили — для оценки пинга
};

// Своя пуля, нарисованная СРАЗУ по клику (предсказание выстрела).
// Сервер про неё узнает только через ~пинг; когда он подтвердит ввод с выстрелом,
// мы проверим, что такая пуля у него действительно есть.
struct PredictedBullet {
    uint32_t seq;          // номер ввода, в котором был выстрел
    Shot shot;             // откуда и с какой скоростью вылетела
    double firedAt;        // когда (по нашим часам)
    bool confirmed;        // сервер подтвердил, что пуля есть
};

// Скорости совпадают → это та же пуля (скорость приходит с точностью 0.01)
bool sameShot(const BulletState& b, const Shot& s) {
    return std::fabs(b.vx - s.vx) < 0.5f && std::fabs(b.vy - s.vy) < 0.5f;
}

// Снимок мира от сервера (для интерполяции)
struct Snapshot {
    uint32_t tick;
    std::vector<PlayerState> players;
    std::vector<BulletState> bullets;
};

// На каком тике сервера пуля появилась (≈ тик выстрела).
// Идём по снимкам от свежих к старым, пока в них есть "та же" пуля: тот же владелец,
// та же скорость и позиция, отмотанная назад по прямой.
double bulletSpawnTick(const BulletState& b, uint32_t tick, const std::deque<Snapshot>& snaps) {
    double spawn = tick;
    for (auto it = snaps.rbegin(); it != snaps.rend(); ++it) {
        if (it->tick > tick) continue;
        float back = (float)(tick - it->tick) / TICK_RATE;
        float ex = b.x - b.vx * back, ey = b.y - b.vy * back;   // где пуля была бы на том тике
        bool found = false;
        for (const auto& o : it->bullets) {
            if (o.owner == b.owner && sameShot(o, {0, 0, b.vx, b.vy}) &&
                std::fabs(o.x - ex) < 2.0f && std::fabs(o.y - ey) < 2.0f) { found = true; break; }
        }
        if (!found) break;
        spawn = it->tick;
    }
    return spawn;
}

void sendPacket(UdpClient& net, const Writer& w) {
    net.send(w.data(), w.size());
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
    // PI — из raylib.h (M_PI из <cmath> есть не во всех компиляторах, на Windows его нет)
    float d = std::remainder(b - a, 2.0f * PI);
    return a + d * t;
}

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : DEFAULT_PORT;

    // ---------- Сеть ----------
    UdpClient net;
    if (!net.open(host, port)) return 1;

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

    // предсказание выстрела
    std::vector<PredictedBullet> myBullets;
    int fireCooldown = 0;            // перезарядка в вводах — считается так же, как на сервере (tryFire)
    double muzzleFlashAt = -10;      // когда был последний свой выстрел (для вспышки у дула)

    double rttMs = 0;                // оценка пинга
    double lastHelloAt = -10;
    double lastPacketAt = GetTime();
    double lastReopenAt = GetTime();  // когда последний раз пересоздавали сокет (при долгой потере связи)

    // Случайный номер сессии: по нему сервер узнает нас, даже если сменился адрес (см. encodeHello)
    std::random_device rd;
    uint32_t sessionToken = 0;
    while (sessionToken == 0) {
        sessionToken = rd() ^ (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
    }

    while (!WindowShouldClose()) {
        double now = GetTime();

        if (IsKeyPressed(KEY_P)) {
            predictionOn = !predictionOn;
            myBullets.clear();   // без предсказания свои пули снова рисуются из снимков сервера
        }
        if (IsKeyPressed(KEY_I)) interpolationOn = !interpolationOn;

        // ---------- 0. Потеря связи ----------
        // Сколько уже ничего не приходит от сервера (до приёма пакетов этого кадра)
        double silence = now - lastPacketAt;
        // Совсем долго тихо — пересоздаём сокет: получим новый локальный порт и новую запись в NAT роутера.
        // Сервер узнает нас по sessionToken в HELLO и перенесёт на новый адрес.
        if (myId != 0 && !gameOver && silence > 3.0 && now - lastReopenAt > 3.0) {
            net.close();
            net.open(host, port);   // не получилось (сеть совсем пропала) — попробуем через 3 с
            lastReopenAt = now;
        }

        // ---------- 1. Приём ----------
        while (true) {
            uint8_t buf[2048];
            int n = net.receive(buf, sizeof(buf));
            if (n <= 0) break;   // пакетов больше нет
            lastPacketAt = now;

            Reader r(buf, (size_t)n);
            uint8_t type = r.u8();

            if (type == SRV_WELCOME) {
                uint8_t id = r.u8();
                if (!r.ok()) continue;
                // Первое подключение или ПЕРЕподключение: всё, что знали о мире, могло устареть.
                // Важно сбросить haveState: если сервер перезапускался, его тики начались с нуля,
                // и без сброса мы бы выбрасывали все новые снимки как "старые" (s.tick <= latest.tick).
                // inputSeq НЕ сбрасываем: сервер (если помнит нас) ждёт номера больше прежних.
                if (myId == 0 || id != myId || silence > 1.0) {
                    haveState = false;
                    snapshots.clear();
                    renderClock = RenderClock{};
                    pending.clear();
                    havePrediction = false;
                    myBullets.clear();
                }
                myId = id;
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

                    // ---- подтверждение своих выстрелов ----
                    // Раунд кончился (кто-то попал) — сервер очистил пули, очищаем и мы.
                    if (s.phase != PHASE_PLAYING) myBullets.clear();
                    // Сервер уже учёл ввод с выстрелом → такая пуля должна быть в снимке.
                    // Нет — значит, сервер выстрел не принял (перезарядка, фаза): убираем свою.
                    for (size_t i = 0; i < myBullets.size(); ) {
                        PredictedBullet& pb = myBullets[i];
                        if (!pb.confirmed && pb.seq <= me->lastInputSeq) {
                            for (const auto& b : s.bullets)
                                if (b.owner == myId && sameShot(b, pb.shot)) pb.confirmed = true;
                            if (!pb.confirmed) { myBullets.erase(myBullets.begin() + i); continue; }
                        }
                        i++;
                    }
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
            // HELLO: пока не подключились, а также если сервер молчит больше 1 с (переподключение).
            // Вводы при этом продолжаем слать — если связь вернулась сама, сервер их сразу примет.
            if ((myId == 0 || now - lastPacketAt > 1.0) && now - lastHelloAt > 0.5) {
                sendPacket(net, encodeHello(sessionToken));
                lastHelloAt = now;
            }
            if (myId != 0) {
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
                    in.aim = quantizeAim(myAim);   // с той же точностью, что уйдёт по сети

                    // ---- PREDICTION: двигаем себя сразу, той же функцией, что и сервер ----
                    if (havePrediction && meServer && canMove(latest.phase, meServer->alive)) {
                        applyMovement(predX, predY, in.keys, INPUT_DT);
                    }

                    // ---- PREDICTION выстрела: то же правило, что на сервере (tryFire) ----
                    // Перезарядку считаем всегда, даже без предсказания, — чтобы не разойтись с сервером.
                    bool allowed = meServer && canShoot(latest.phase, meServer->alive);
                    if (tryFire(fireCooldown, in.keys, allowed) && predictionOn && havePrediction) {
                        myBullets.push_back({in.seq, makeShot(predX, predY, in.aim), now, false});
                        muzzleFlashAt = now;
                    }

                    pending.push_back({in, now});
                    while (pending.size() > 120) pending.pop_front();   // сервер давно молчит — не копим бесконечно
                    // шлём этот ввод + несколько предыдущих (на случай потери пакетов)
                    InputMsg recent[INPUT_REDUNDANCY];
                    int count = 0;
                    size_t first = pending.size() > (size_t)INPUT_REDUNDANCY ? pending.size() - INPUT_REDUNDANCY : 0;
                    for (size_t i = first; i < pending.size(); i++) recent[count++] = pending[i].in;
                    sendPacket(net, encodeInput(recent, count));
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
            //
            // С предсказанием свои пули рисуем из myBullets: они вылетели из ствола в момент клика
            // и летят сами. Серверные свои пули рисуем, только если у них нет предсказанной пары
            // (например, выстрел пришёлся на начало раунда, которое клиент ещё не увидел).
            for (size_t i = 0; i < myBullets.size(); ) {
                const PredictedBullet& pb = myBullets[i];
                float age = (float)(now - pb.firedAt);
                float bx = pb.shot.x + pb.shot.vx * age, by = pb.shot.y + pb.shot.vy * age;
                if (age > BULLET_LIFETIME || bx < 0 || by < 0 || bx > MAP_SIZE || by > MAP_SIZE) {
                    myBullets.erase(myBullets.begin() + i);   // улетела за стену — как и на сервере
                    continue;
                }
                DrawCircleV(Vector2{bx, by + HUD_HEIGHT}, BULLET_RADIUS, DARKBLUE);
                i++;
            }
            float flight = (float)std::min(now - latestAt, 0.3);
            for (const auto& b : latest.bullets) {
                if (b.owner != myId) continue;
                bool predicted = false;
                for (const auto& pb : myBullets) if (pb.confirmed && sameShot(b, pb.shot)) predicted = true;
                if (predicted) continue;   // её уже нарисовали выше — из ствола, без задержки
                float bx = b.x + b.vx * flight, by = b.y + b.vy * flight;
                if (bx < 0 || by < 0 || bx > MAP_SIZE || by > MAP_SIZE) continue;   // уже улетела за стену
                DrawCircleV(Vector2{bx, by + HUD_HEIGHT}, BULLET_RADIUS, DARKBLUE);
            }
            // ЧУЖИЕ ПУЛИ: в каком моменте времени их рисовать.
            // На экране два "времени":
            //   enemyTick — в нём рисуем соперника (интерполяция, в прошлом на ping/2 + interp);
            //   myTick    — в нём рисуем себя (предсказание: latest.tick + неподтверждённые вводы).
            // Между ними ping + interp — пуля успевает пролететь сотню единиц.
            // Попадание решается только РЯДОМ С ТОБОЙ, поэтому:
            //   у дула врага пуля в enemyTick — вылетает из ствола, когда он стреляет на экране;
            //   подлетая к тебе, плавно переходит в myTick (чуть ускоряется);
            //   в точке, где ближе всего к тебе, она уже целиком в myTick — касание = попадание на сервере.
            // Пуля летит по прямой, поэтому её позицию на любом тике можно посчитать точно.
            const double latestTick = latest.tick;
            // Не PLAYING (пауза, отсчёт) — пули на сервере стоят, рисуем их там, где стоят
            const bool flying = latest.phase == PHASE_PLAYING;
            const double enemyTick = (interpolationOn && flying) ? std::min(renderTick, latestTick) : latestTick;
            const double myTick = (predictionOn && havePrediction && flying) ? latestTick + (double)pending.size() : latestTick;
            const float BLEND_DIST = 250.0f;   // за сколько единиц до тебя пуля начинает "догонять" твоё время

            for (const auto& b : latest.bullets) {
                if (b.owner == myId) continue;
                float speed = std::hypot(b.vx, b.vy);
                if (speed < 1.0f) continue;
                float dirX = b.vx / speed, dirY = b.vy / speed;
                auto posAt = [&](double tick, float& x, float& y) {
                    float dt = (float)((tick - latestTick) / TICK_RATE);
                    x = b.x + b.vx * dt;
                    y = b.y + b.vy * dt;
                };

                double spawnTick = bulletSpawnTick(b, latest.tick, snapshots);

                // s — сколько пуле ещё лететь до точки, где она ближе всего к тебе (в твоём времени).
                // Летит мимо/от тебя с самого вылета — переход не нужен, рисуем в времени соперника.
                float sx, sy, mx, my;
                posAt(spawnTick, sx, sy);
                posAt(myTick, mx, my);
                float sFromSpawn = (myX - sx) * dirX + (myY - sy) * dirY;
                float s = (myX - mx) * dirX + (myY - my) * dirY;
                float w = 0;
                if (sFromSpawn > 0) {
                    w = s <= 0 ? 1.0f : std::clamp(1.0f - s / BLEND_DIST, 0.0f, 1.0f);
                    w = w * w * (3 - 2 * w);   // smoothstep: ускорение без рывка
                }

                double drawTick = enemyTick + (myTick - enemyTick) * w;
                if (drawTick < spawnTick) continue;   // в этом моменте враг ещё не выстрелил
                float bx, by;
                posAt(drawTick, bx, by);
                if (bx < 0 || by < 0 || bx > MAP_SIZE || by > MAP_SIZE) continue;   // уже улетела за стену
                DrawCircleV(Vector2{bx, by + HUD_HEIGHT}, BULLET_RADIUS, MAROON);
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
                Vector2 tip{pos.x + std::cos(aim) * GUN_LENGTH,
                            pos.y + std::sin(aim) * GUN_LENGTH};
                DrawLineEx(pos, tip, 4, DARKGRAY);
                // вспышка выстрела у дула — в тот же кадр, что и клик
                if (p.id == myId && now - muzzleFlashAt < 0.06) {
                    float k = 1.0f - (float)((now - muzzleFlashAt) / 0.06);
                    DrawCircleV(tip, 6 + 4 * k, Fade(ORANGE, 0.9f * k));
                }
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
            drawCentered("Connection lost, reconnecting...", midY, 36, MAROON);
            drawCentered(TextFormat("no data for %.0f s (server waits %d s)", now - lastPacketAt, PLAYER_TIMEOUT_SEC),
                         midY + 50, 20, GRAY);
        } else if (haveState) {
            if (latest.phase == PHASE_WAITING) {
                drawCentered("Waiting for opponent...", midY, 30, DARKGRAY);
            } else if (latest.phase == PHASE_COUNTDOWN) {
                drawCentered(TextFormat("Round %d", latest.round), midY, 50, DARKGRAY);
                drawCentered(TextFormat("%.1f", latest.timeLeftMs / 1000.0f), midY + 60, 30, GRAY);
            } else if (latest.phase == PHASE_PAUSED) {
                drawCentered("PAUSED", midY, 50, DARKGRAY);
                if (latest.timeLeftMs == 0)
                    drawCentered("A player lost connection, waiting...", midY + 60, 24, GRAY);
                else
                    drawCentered(TextFormat("Connection is back. Resuming in %.1f", latest.timeLeftMs / 1000.0f),
                                 midY + 60, 24, DARKGREEN);
            } else if (latest.phase == PHASE_ROUND_OVER) {
                const char* text = latest.lastScorer == 0    ? "Both hit!"
                                 : latest.lastScorer == myId ? "You hit!"
                                                             : "You were hit!";
                drawCentered(text, midY, 50, latest.lastScorer == myId ? DARKGREEN : MAROON);
            }
        }

        EndDrawing();
    }

    if (myId != 0 && !gameOver) sendPacket(net, encodeLeave());
    CloseWindow();
    net.close();
    return 0;
}
