// Общий протокол для клиента и сервера варианта 3 (v3_with_end_game).
//
// Клиент → сервер (13 байт, все числа в сетевом порядке байт, big-endian):
//   [0]       uint8   type        MSG_HELLO или MSG_VALUE
//   [1..4]    int32   value       число игрока (для HELLO не используется)
//   [5..12]   uint64  sendTimeUs  время отправки по UTC, микросекунды с 1970-01-01
//
// Сервер → клиент:
//   [0]       uint8   type        SRV_STATE или SRV_GAME_END
//   SRV_STATE:    [1..]  текст состояния (просто печатаем)
//   SRV_GAME_END: [1]    uint8 result (RESULT_LOSE / RESULT_WIN / RESULT_DRAW)
//                 [2..]  текст с итогом игры

#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <string>

inline constexpr uint16_t DEFAULT_PORT = 7777;

enum MsgType : uint8_t {
    MSG_HELLO = 1,   // "я здесь": первое сообщение и периодический keepalive
    MSG_VALUE = 2,   // игрок прислал новое число
};

enum ServerMsgType : uint8_t {
    SRV_STATE    = 10,   // обновление состояния
    SRV_GAME_END = 11,   // игра окончена: результат, после этого клиент отключается
};

enum GameResult : uint8_t {
    RESULT_LOSE = 0,
    RESULT_WIN  = 1,
    RESULT_DRAW = 2,
};

inline constexpr size_t CLIENT_MSG_SIZE = 13;

struct ClientMsg {
    uint8_t type = MSG_HELLO;
    int32_t value = 0;
    uint64_t sendTimeUs = 0;
};

// --- Запись и чтение чисел побайтово в big-endian ---
// Так формат одинаков на любом процессоре, и не нужны htonl/ntohl.

inline void writeU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

inline uint32_t readU32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

inline void writeU64(uint8_t* p, uint64_t v) {
    writeU32(p, (uint32_t)(v >> 32));       // старшие 4 байта
    writeU32(p + 4, (uint32_t)v);           // младшие 4 байта
}

inline uint64_t readU64(const uint8_t* p) {
    return ((uint64_t)readU32(p) << 32) | readU32(p + 4);
}

// --- Упаковка и распаковка сообщения клиента ---

inline void encodeClientMsg(const ClientMsg& m, uint8_t out[CLIENT_MSG_SIZE]) {
    out[0] = m.type;
    writeU32(out + 1, (uint32_t)m.value);
    writeU64(out + 5, m.sendTimeUs);
}

// false, если пакет неправильного размера или неизвестного типа
inline bool decodeClientMsg(const uint8_t* buf, size_t len, ClientMsg& m) {
    if (len != CLIENT_MSG_SIZE) return false;
    if (buf[0] != MSG_HELLO && buf[0] != MSG_VALUE) return false;
    m.type = buf[0];
    m.value = (int32_t)readU32(buf + 1);
    m.sendTimeUs = readU64(buf + 5);
    return true;
}

// --- Время по UTC ---

// Текущее время UTC в микросекундах с 1970-01-01 (Unix time).
// system_clock — "настоящие" часы (в отличие от steady_clock, который
// годится только для измерения интервалов).
inline uint64_t nowUtcUs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

// 1731600000123456 → "2024-11-14 16:00:00.123 UTC"
inline std::string formatUtc(uint64_t us) {
    time_t seconds = (time_t)(us / 1000000);
    unsigned millis = (unsigned)((us % 1000000) / 1000);

    tm t{};
#if defined(_WIN32)
    gmtime_s(&t, &seconds);
#else
    gmtime_r(&seconds, &t);
#endif                 // разложить секунды на дату/время по UTC

    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &t);

    char out[48];
    snprintf(out, sizeof(out), "%s.%03u UTC", date, millis);
    return out;
}
