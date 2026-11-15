// Вариант 5 (v5_prediction): общий протокол, константы и ОБЩИЙ КОД ДВИЖЕНИЯ.
//
// Игра та же, что в v4: квадратная карта, 2 игрока, WASD + мышь, 3 раунда.
//
// Новое в v5 — client-side prediction (как в CS2 и других шутерах):
//   - каждый INPUT означает ровно 1/60 секунды движения (INPUT_DT);
//   - клиент СРАЗУ двигает своего персонажа функцией applyMovement() — без ожидания сервера;
//   - сервер прогоняет те же INPUT той же функцией applyMovement() — это "настоящая" позиция;
//   - в состоянии сервер сообщает, какой последний INPUT он учёл (lastInputSeq),
//     клиент сверяется и при расхождении поправляет себя (reconciliation).
// Сервер по-прежнему единственный "судья" — клиент только предсказывает.
//
// Сервер ничего не рисует: ему нужны только числа (позиции, пули, попадания).
//
// Все числа в пакетах — big-endian. Дробные числа (координаты, угол) передаются
// как int32 = значение * 100 (точность 0.01).
//
// Клиент → сервер:
//   MSG_HELLO: [u8 type]                                   "хочу играть" (шлём, пока не придёт WELCOME)
//   MSG_INPUT: [u8 type][u32 seq][u8 keys][f32 aim]        ввод, 60 раз в секунду
//   MSG_LEAVE: [u8 type]                                   "выхожу" (закрыл окно)
//
// Сервер → клиент:
//   SRV_WELCOME:  [u8 type][u8 yourId]                     "ты в игре, твой номер такой-то"
//   SRV_FULL:     [u8 type]                                "мест нет"
//   SRV_STATE:    [u8 type][u32 tick][u8 phase][u8 round][u8 roundsTotal][u16 timeLeftMs]
//                 [u8 lastScorer]
//                 [u8 playerCount] { [u8 id][f32 x][f32 y][f32 aim][u8 score][u8 alive][u32 lastInputSeq] } ...
//                 [u8 bulletCount] { [f32 x][f32 y][u8 owner] } ...
//   SRV_GAME_END: [u8 type][u8 winnerId (0 = ничья)][u8 count] { [u8 id][u8 score] } ...

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <string>
#include <vector>

// ---------- Константы игры (одинаковые на клиенте и сервере) ----------

inline constexpr uint16_t DEFAULT_PORT = 7777;

inline constexpr float MAP_SIZE        = 800.0f;   // карта MAP_SIZE x MAP_SIZE
inline constexpr float PLAYER_RADIUS   = 16.0f;
inline constexpr float PLAYER_SPEED    = 260.0f;   // единиц карты в секунду
inline constexpr float BULLET_RADIUS   = 4.0f;
inline constexpr float BULLET_SPEED    = 450.0f;   // единиц в секунду: через всю карту ~1.8 с, можно увернуться
inline constexpr float BULLET_LIFETIME = 2.0f;     // секунд (450 * 2 = 900 > размера карты)
inline constexpr float SHOOT_COOLDOWN  = 0.35f;    // секунд между выстрелами

inline constexpr int TICK_RATE    = 60;   // сколько раз в секунду сервер обновляет мир
inline constexpr int ROUNDS_TOTAL = 3;
inline constexpr int MAX_PLAYERS  = 2;
inline constexpr int MAX_BULLETS  = 64;

inline constexpr float COUNTDOWN_TIME  = 1.5f;    // пауза перед началом раунда
inline constexpr float ROUND_OVER_TIME = 2.0f;    // пауза после попадания

// Один INPUT = ровно столько секунд движения. Клиент шлёт TICK_RATE (60) вводов в секунду.
inline constexpr float INPUT_DT = 1.0f / TICK_RATE;

// "Бюджет" вводов на сервере копится по РЕАЛЬНОМУ времени: +1 ввод за каждые 1/60 с.
// Если вводы какое-то время не приходили (клиент подвис, сеть задержала пакеты),
// бюджет накапливается до MAX_INPUT_BURST, и пришедшая потом пачка применяется сразу —
// задержка не "застревает". В среднем быстрее реального времени двигаться всё равно нельзя
// (защита от speedhack: присылать вводы чаще бессмысленно).
inline constexpr int MAX_INPUT_BURST = 30;    // ~0.5 с: Wi-Fi иногда залипает на 200–400 мс

// Если в очереди накопилось больше — выбрасываем САМЫЕ СТАРЫЕ вводы,
// чтобы задержка управления не могла вырасти выше ~0.67 с.
inline constexpr int MAX_QUEUED_INPUTS = 40;

// Вводы, на которые нет бюджета, — это "хвост" после задержки сети дольше MAX_INPUT_BURST:
// приток = отток, и сам он НИКОГДА не рассосётся (каждый следующий ввод будет ждать весь хвост).
// Оставляем в очереди не больше (бюджет + MAX_INPUT_BACKLOG).
// Поэтому лишнее выбрасываем: игрок увидит маленький рывок (сервер не применил эти шаги),
// зато задержка управления сразу возвращается к норме.
inline constexpr int MAX_INPUT_BACKLOG = 2;   // ~33 мс запаса на неровный приход пакетов

// Сколько вводов максимум применяем за ОДИН тик. Если пачка опоздавших вводов
// применить разом, игрок у всех остальных телепортируется на десятки пикселей.
// По 3 за тик: он догоняет за несколько тиков (быстро, но плавно).
inline constexpr int MAX_INPUTS_PER_TICK = 3;

// Буфер вводов на сервере (см. processInputs): максимум 12 тиков = 200 мс.
inline constexpr int MAX_INPUT_BUFFER = 12;

// ---------- Ввод: биты в одном байте ----------

enum InputKeys : uint8_t {
    IN_UP    = 1 << 0,
    IN_DOWN  = 1 << 1,
    IN_LEFT  = 1 << 2,
    IN_RIGHT = 1 << 3,
    IN_FIRE  = 1 << 4,
};

// ---------- Типы сообщений (первый байт пакета) ----------

enum ClientMsgType : uint8_t {
    MSG_HELLO = 1,
    MSG_INPUT = 2,
    MSG_LEAVE = 3,
};

enum ServerMsgType : uint8_t {
    SRV_WELCOME  = 20,
    SRV_STATE    = 21,
    SRV_GAME_END = 22,
    SRV_FULL     = 23,
};

// ---------- Фазы игры ----------

enum Phase : uint8_t {
    PHASE_WAITING    = 0,   // ждём второго игрока
    PHASE_COUNTDOWN  = 1,   // "Round N", все стоят
    PHASE_PLAYING    = 2,   // можно двигаться и стрелять
    PHASE_ROUND_OVER = 3,   // кто-то попал, короткая пауза
};

// ---------- ОБЩИЙ КОД ДВИЖЕНИЯ: одинаковый на клиенте и сервере ----------
// Именно потому, что код один и тот же, предсказание клиента почти всегда
// совпадает с тем, что посчитает сервер.

// В каких фазах игрок может двигаться
inline bool canMove(uint8_t phase, bool alive) {
    return alive && (phase == PHASE_WAITING || phase == PHASE_PLAYING);
}

// Сдвинуть игрока по нажатым клавишам на dt секунд
inline void applyMovement(float& x, float& y, uint8_t keys, float dt) {
    float dx = ((keys & IN_RIGHT) ? 1.0f : 0.0f) - ((keys & IN_LEFT) ? 1.0f : 0.0f);
    float dy = ((keys & IN_DOWN)  ? 1.0f : 0.0f) - ((keys & IN_UP)   ? 1.0f : 0.0f);
    float len = std::sqrt(dx * dx + dy * dy);
    if (len > 0) {
        // делим на длину, чтобы по диагонали не бегать быстрее
        x += dx / len * PLAYER_SPEED * dt;
        y += dy / len * PLAYER_SPEED * dt;
    }
    // не выходим за края карты
    x = std::clamp(x, PLAYER_RADIUS, MAP_SIZE - PLAYER_RADIUS);
    y = std::clamp(y, PLAYER_RADIUS, MAP_SIZE - PLAYER_RADIUS);
}

// ---------- Запись и чтение пакетов ----------

// Складывает числа в буфер байт по порядку (big-endian).
class Writer {
public:
    void u8(uint8_t v)   { buf.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)(v >> 8)); u8((uint8_t)v); }
    void u32(uint32_t v) { u16((uint16_t)(v >> 16)); u16((uint16_t)v); }
    // дробное число → int32 с точностью 0.01
    void f32(float v)    { u32((uint32_t)(int32_t)std::lround(v * 100.0f)); }

    const uint8_t* data() const { return buf.data(); }
    size_t size() const { return buf.size(); }

private:
    std::vector<uint8_t> buf;
};

// Читает числа из буфера по порядку. Если данных не хватило — ok() станет false
// (так мы не читаем за пределы пакета, даже если пришёл мусор).
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : p(data), end(data + size) {}

    bool ok() const { return good; }

    uint8_t u8() {
        if (p >= end) { good = false; return 0; }
        return *p++;
    }
    uint16_t u16() { uint16_t hi = u8(); uint16_t lo = u8(); return (uint16_t)((hi << 8) | lo); }
    uint32_t u32() { uint32_t hi = u16(); uint32_t lo = u16(); return (hi << 16) | lo; }
    float f32()    { return (float)(int32_t)u32() / 100.0f; }

private:
    const uint8_t* p;
    const uint8_t* end;
    bool good = true;
};

// ---------- Сообщения клиента ----------

struct InputMsg {
    uint32_t seq = 0;    // номер пакета: старые (пришедшие не по порядку) сервер выбросит
    uint8_t keys = 0;    // биты InputKeys
    float aim = 0;       // угол прицела в радианах (0 = вправо, по часовой, т.к. ось Y вниз)
};

inline Writer encodeHello() { Writer w; w.u8(MSG_HELLO); return w; }
inline Writer encodeLeave() { Writer w; w.u8(MSG_LEAVE); return w; }

// В одном пакете INPUT лежат несколько ПОСЛЕДНИХ вводов (старые первыми).
// UDP иногда теряет пакеты; если пакет с вводом №10 потерялся, ввод №10 приедет
// ещё раз в пакетах №11, №12, №13 — и движение не пропадёт. Сервер берёт только новые (seq больше уже полученного).
// Цена — пакет 1 + 1 + 4×9 = 38 байт вместо 10. Для 60 пакетов/с это копейки.
inline constexpr int INPUT_REDUNDANCY = 4;

inline Writer encodeInput(const InputMsg* inputs, int count) {
    Writer w;
    w.u8(MSG_INPUT);
    w.u8((uint8_t)count);
    for (int i = 0; i < count; i++) {
        w.u32(inputs[i].seq);
        w.u8(inputs[i].keys);
        w.f32(inputs[i].aim);
    }
    return w;
}
inline Writer encodeInput(const InputMsg& m) { return encodeInput(&m, 1); }

// Читает всё ПОСЛЕ байта типа
inline bool decodeInput(Reader& r, std::vector<InputMsg>& out) {
    uint8_t count = r.u8();
    if (count == 0 || count > INPUT_REDUNDANCY) return false;
    out.resize(count);
    for (auto& m : out) {
        m.seq = r.u32();
        m.keys = r.u8();
        m.aim = r.f32();
    }
    return r.ok();
}

// ---------- Сообщения сервера ----------

struct PlayerState {
    uint8_t id = 0;
    float x = 0, y = 0, aim = 0;
    uint8_t score = 0;
    uint8_t alive = 1;
    uint32_t lastInputSeq = 0;   // последний INPUT этого игрока, который сервер уже применил
};

struct BulletState {
    float x = 0, y = 0;
    float vx = 0, vy = 0;   // скорость: клиент сам двигает пулю между пакетами
    uint8_t owner = 0;
};

struct StateMsg {
    uint32_t tick = 0;          // номер тика сервера: по нему клиент отбрасывает устаревшие пакеты
    uint8_t phase = PHASE_WAITING;
    uint8_t round = 0;
    uint8_t roundsTotal = ROUNDS_TOTAL;
    uint16_t timeLeftMs = 0;    // сколько осталось до конца паузы (для отсчёта на экране)
    uint8_t lastScorer = 0;     // кто попал в прошлом раунде (0 = оба / никто)
    std::vector<PlayerState> players;
    std::vector<BulletState> bullets;
};

struct PlayerScore {
    uint8_t id = 0;
    uint8_t score = 0;
};

struct GameEndMsg {
    uint8_t winnerId = 0;       // 0 = ничья
    std::vector<PlayerScore> scores;
};

inline Writer encodeWelcome(uint8_t yourId) { Writer w; w.u8(SRV_WELCOME); w.u8(yourId); return w; }
inline Writer encodeFull() { Writer w; w.u8(SRV_FULL); return w; }

inline Writer encodeState(const StateMsg& s) {
    Writer w;
    w.u8(SRV_STATE);
    w.u32(s.tick);
    w.u8(s.phase);
    w.u8(s.round);
    w.u8(s.roundsTotal);
    w.u16(s.timeLeftMs);
    w.u8(s.lastScorer);

    w.u8((uint8_t)s.players.size());
    for (const auto& p : s.players) {
        w.u8(p.id);
        w.f32(p.x);
        w.f32(p.y);
        w.f32(p.aim);
        w.u8(p.score);
        w.u8(p.alive);
        w.u32(p.lastInputSeq);
    }

    w.u8((uint8_t)s.bullets.size());
    for (const auto& b : s.bullets) {
        w.f32(b.x);
        w.f32(b.y);
        w.f32(b.vx);
        w.f32(b.vy);
        w.u8(b.owner);
    }
    return w;
}

inline bool decodeState(Reader& r, StateMsg& s) {
    s.tick = r.u32();
    s.phase = r.u8();
    s.round = r.u8();
    s.roundsTotal = r.u8();
    s.timeLeftMs = r.u16();
    s.lastScorer = r.u8();

    uint8_t playerCount = r.u8();
    if (playerCount > MAX_PLAYERS) return false;   // защита от мусора
    s.players.resize(playerCount);
    for (auto& p : s.players) {
        p.id = r.u8();
        p.x = r.f32();
        p.y = r.f32();
        p.aim = r.f32();
        p.score = r.u8();
        p.alive = r.u8();
        p.lastInputSeq = r.u32();
    }

    uint8_t bulletCount = r.u8();
    if (bulletCount > MAX_BULLETS) return false;
    s.bullets.resize(bulletCount);
    for (auto& b : s.bullets) {
        b.x = r.f32();
        b.y = r.f32();
        b.vx = r.f32();
        b.vy = r.f32();
        b.owner = r.u8();
    }
    return r.ok();
}

inline Writer encodeGameEnd(const GameEndMsg& g) {
    Writer w;
    w.u8(SRV_GAME_END);
    w.u8(g.winnerId);
    w.u8((uint8_t)g.scores.size());
    for (const auto& s : g.scores) {
        w.u8(s.id);
        w.u8(s.score);
    }
    return w;
}

inline bool decodeGameEnd(Reader& r, GameEndMsg& g) {
    g.winnerId = r.u8();
    uint8_t count = r.u8();
    if (count > MAX_PLAYERS) return false;
    g.scores.resize(count);
    for (auto& s : g.scores) {
        s.id = r.u8();
        s.score = r.u8();
    }
    return r.ok();
}

// ---------- Время по UTC (для логов сервера) ----------

inline uint64_t nowUtcUs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

inline std::string formatUtc(uint64_t us) {
    time_t seconds = (time_t)(us / 1000000);
    unsigned millis = (unsigned)((us % 1000000) / 1000);
    tm t{};
#if defined(_WIN32)
    gmtime_s(&t, &seconds);
#else
    gmtime_r(&seconds, &t);
#endif
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &t);
    char out[48];
    snprintf(out, sizeof(out), "%s.%03u UTC", date, millis);
    return out;
}
