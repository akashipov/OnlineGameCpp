// Вариант 4 (v4_shooter): сервер реалтайм-игры "стрелялка на квадратной карте".
//
// Как работает:
//   - сервер живёт в игровом цикле TICK_RATE раз в секунду (60);
//   - между тиками принимает пакеты клиентов: HELLO (подключение), INPUT (клавиши + прицел), LEAVE;
//   - каждый тик: двигает игроков по их последнему вводу, создаёт и двигает пули,
//     проверяет попадания, переключает фазы раунда;
//   - после каждого тика рассылает ВСЕМ клиентам полное состояние мира (SRV_STATE).
//
// Раунды: попал в соперника → +1 очко, пауза, новый раунд. После ROUNDS_TOTAL раундов —
// SRV_GAME_END обоим, игроки удаляются, сервер ждёт новых.
//
// Запуск:  ./v4_server [port]      (по умолчанию 7777)

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <csignal>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>

#include "../common/protocol.h"

using Clock = std::chrono::steady_clock;

static volatile sig_atomic_t lastSignal = 0;
void onSignal(int sig) { lastSignal = sig; }

struct Player {
    uint8_t id = 0;              // 1 или 2
    sockaddr_in addr{};
    float x = 0, y = 0, aim = 0;
    uint8_t keys = 0;            // последний присланный ввод
    uint32_t lastSeq = 0;        // номер последнего принятого INPUT
    uint8_t score = 0;
    bool alive = true;
    float cooldown = 0;          // сколько ждать до следующего выстрела
    Clock::time_point lastSeen;
};

struct Bullet {
    float x, y, vx, vy;
    float life;                  // сколько секунд ещё летит
    uint8_t owner;               // id стрелявшего
};

uint64_t makeKey(const sockaddr_in& a) {
    return ((uint64_t)a.sin_addr.s_addr << 16) | a.sin_port;
}

std::string addrToString(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

void logLine(const std::string& text) {
    printf("[%s] %s\n", formatUtc(nowUtcUs()).c_str(), text.c_str());
    fflush(stdout);
}

class GameServer {
public:
    explicit GameServer(int sock) : sock_(sock) {}

    // ---------- Сеть: входящие пакеты ----------

    void handlePacket(const uint8_t* data, size_t size, const sockaddr_in& from) {
        Reader r(data, size);
        uint8_t type = r.u8();
        if (!r.ok()) return;

        uint64_t key = makeKey(from);
        auto it = players_.find(key);

        if (type == MSG_HELLO) {
            if (it != players_.end()) {
                send(from, encodeWelcome(it->second.id));   // WELCOME мог потеряться — повторяем
                return;
            }
            if ((int)players_.size() >= MAX_PLAYERS) {
                send(from, encodeFull());
                return;
            }
            addPlayer(key, from);
            return;
        }

        if (it == players_.end()) return;   // пакет от незнакомца — игнорируем
        Player& p = it->second;
        p.lastSeen = Clock::now();

        if (type == MSG_INPUT) {
            InputMsg in;
            if (!decodeInput(r, in)) return;
            if (in.seq <= p.lastSeq) return;   // старый пакет пришёл позже нового — выбрасываем
            p.lastSeq = in.seq;
            p.keys = in.keys;
            if (std::isfinite(in.aim)) p.aim = in.aim;
        } else if (type == MSG_LEAVE) {
            removePlayer(key, "left the game");
        }
    }

    // ---------- Один тик игры ----------

    void update(float dt) {
        tick_++;
        dropSilentPlayers();

        switch (phase_) {
            case PHASE_WAITING:
                movePlayers(dt);   // пока ждём соперника, можно побегать, но не стрелять
                break;

            case PHASE_COUNTDOWN:
                phaseTimer_ -= dt;
                if (phaseTimer_ <= 0) phase_ = PHASE_PLAYING;
                break;

            case PHASE_PLAYING:
                movePlayers(dt);
                shoot();
                moveBullets(dt);
                checkHits();
                break;

            case PHASE_ROUND_OVER:
                phaseTimer_ -= dt;
                if (phaseTimer_ <= 0) {
                    if (round_ >= ROUNDS_TOTAL) endGame();
                    else { round_++; startRound(); }
                }
                break;
        }
    }

    // ---------- Рассылка состояния всем ----------

    void broadcastState() {
        StateMsg s;
        s.tick = tick_;
        s.phase = phase_;
        s.round = (uint8_t)round_;
        s.roundsTotal = ROUNDS_TOTAL;
        s.timeLeftMs = (uint16_t)std::max(0.0f, phaseTimer_ * 1000.0f);
        s.lastScorer = lastScorer_;

        for (const auto& [key, p] : players_) {
            s.players.push_back({p.id, p.x, p.y, p.aim, p.score, (uint8_t)(p.alive ? 1 : 0)});
        }
        for (const auto& b : bullets_) {
            s.bullets.push_back({b.x, b.y, b.owner});
        }

        Writer w = encodeState(s);
        for (const auto& [key, p] : players_) send(p.addr, w);
    }

private:
    void send(const sockaddr_in& to, const Writer& w) {
        sendto(sock_, w.data(), w.size(), 0, (const sockaddr*)&to, sizeof(to));
    }

    // ---------- Игроки ----------

    void addPlayer(uint64_t key, const sockaddr_in& from) {
        // свободный номер: 1, если не занят, иначе 2
        uint8_t id = 1;
        for (const auto& [k, p] : players_) if (p.id == 1) id = 2;

        Player p;
        p.id = id;
        p.addr = from;
        p.lastSeen = Clock::now();
        spawn(p);
        players_[key] = p;

        logLine("P" + std::to_string(id) + " joined from " + addrToString(from));
        send(from, encodeWelcome(id));

        if ((int)players_.size() == MAX_PLAYERS && phase_ == PHASE_WAITING) startGame();
    }

    void removePlayer(uint64_t key, const std::string& reason) {
        auto it = players_.find(key);
        if (it == players_.end()) return;
        logLine("P" + std::to_string(it->second.id) + " " + reason);
        players_.erase(it);

        // Игрок ушёл посреди игры — оставшийся побеждает
        if (phase_ != PHASE_WAITING) endGame();
    }

    // UDP не сообщает об отключении: если от игрока 5 секунд ничего нет — он ушёл
    void dropSilentPlayers() {
        auto now = Clock::now();
        std::vector<uint64_t> silent;
        for (const auto& [key, p] : players_) {
            if (now - p.lastSeen > std::chrono::seconds(5)) silent.push_back(key);
        }
        for (uint64_t key : silent) removePlayer(key, "timed out");
    }

    // Стартовые позиции: игроки напротив друг друга
    void spawn(Player& p) {
        p.y = MAP_SIZE / 2;
        if (p.id == 1) { p.x = 150;            p.aim = 0; }          // смотрит вправо
        else           { p.x = MAP_SIZE - 150; p.aim = (float)M_PI; } // смотрит влево
        p.alive = true;
        p.cooldown = 0;
    }

    // ---------- Раунды ----------

    void startGame() {
        for (auto& [key, p] : players_) p.score = 0;
        round_ = 1;
        logLine("Game started");
        startRound();
    }

    void startRound() {
        for (auto& [key, p] : players_) spawn(p);
        bullets_.clear();
        lastScorer_ = 0;
        phase_ = PHASE_COUNTDOWN;
        phaseTimer_ = COUNTDOWN_TIME;
        logLine("Round " + std::to_string(round_) + " of " + std::to_string(ROUNDS_TOTAL));
    }

    void endGame() {
        GameEndMsg g;
        int best = -1;
        for (const auto& [key, p] : players_) {
            g.scores.push_back({p.id, p.score});
            if (p.score > best)       { best = p.score; g.winnerId = p.id; }
            else if (p.score == best) { g.winnerId = 0; }   // одинаково очков — ничья
        }

        std::string text = "GAME OVER:";
        for (const auto& s : g.scores) text += " P" + std::to_string(s.id) + "=" + std::to_string(s.score);
        text += g.winnerId ? " → P" + std::to_string(g.winnerId) + " wins" : " → draw";
        logLine(text);

        // Важное сообщение по UDP — шлём несколько раз на случай потери
        Writer w = encodeGameEnd(g);
        for (const auto& [key, p] : players_) {
            for (int i = 0; i < 3; i++) send(p.addr, w);
        }

        players_.clear();
        bullets_.clear();
        phase_ = PHASE_WAITING;
        round_ = 0;
        phaseTimer_ = 0;
    }

    // ---------- Физика ----------

    void movePlayers(float dt) {
        for (auto& [key, p] : players_) {
            if (!p.alive) continue;

            float dx = ((p.keys & IN_RIGHT) ? 1.0f : 0.0f) - ((p.keys & IN_LEFT) ? 1.0f : 0.0f);
            float dy = ((p.keys & IN_DOWN)  ? 1.0f : 0.0f) - ((p.keys & IN_UP)   ? 1.0f : 0.0f);
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0) {
                // делим на длину, чтобы по диагонали не бегать быстрее
                p.x += dx / len * PLAYER_SPEED * dt;
                p.y += dy / len * PLAYER_SPEED * dt;
            }
            // не выходим за края карты
            p.x = std::clamp(p.x, PLAYER_RADIUS, MAP_SIZE - PLAYER_RADIUS);
            p.y = std::clamp(p.y, PLAYER_RADIUS, MAP_SIZE - PLAYER_RADIUS);

            p.cooldown = std::max(0.0f, p.cooldown - dt);
        }
    }

    void shoot() {
        for (auto& [key, p] : players_) {
            if (!p.alive || !(p.keys & IN_FIRE) || p.cooldown > 0) continue;
            if ((int)bullets_.size() >= MAX_BULLETS) return;

            float cx = std::cos(p.aim), cy = std::sin(p.aim);
            float start = PLAYER_RADIUS + BULLET_RADIUS + 2;   // пуля появляется перед игроком
            bullets_.push_back({p.x + cx * start, p.y + cy * start,
                                cx * BULLET_SPEED, cy * BULLET_SPEED,
                                BULLET_LIFETIME, p.id});
            p.cooldown = SHOOT_COOLDOWN;
        }
    }

    void moveBullets(float dt) {
        for (auto& b : bullets_) {
            b.x += b.vx * dt;
            b.y += b.vy * dt;
            b.life -= dt;
        }
        // удаляем отлетавшие и вылетевшие за карту
        bullets_.erase(std::remove_if(bullets_.begin(), bullets_.end(), [](const Bullet& b) {
            return b.life <= 0 || b.x < 0 || b.y < 0 || b.x > MAP_SIZE || b.y > MAP_SIZE;
        }), bullets_.end());
    }

    void checkHits() {
        const float hitDist = PLAYER_RADIUS + BULLET_RADIUS;
        std::vector<uint8_t> scorers;   // кто попал в этот тик

        for (auto& b : bullets_) {
            for (auto& [key, p] : players_) {
                if (!p.alive || p.id == b.owner) continue;
                float dx = b.x - p.x, dy = b.y - p.y;
                if (dx * dx + dy * dy <= hitDist * hitDist) {
                    p.alive = false;
                    b.life = 0;
                    if (std::find(scorers.begin(), scorers.end(), b.owner) == scorers.end()) {
                        scorers.push_back(b.owner);
                    }
                }
            }
        }
        if (scorers.empty()) return;

        // Начисляем очки. Если попали друг в друга в один тик — очко обоим.
        for (auto& [key, p] : players_) {
            if (std::find(scorers.begin(), scorers.end(), p.id) != scorers.end()) p.score++;
        }
        lastScorer_ = scorers.size() == 1 ? scorers[0] : 0;

        std::string text = "Round " + std::to_string(round_) + ": ";
        text += lastScorer_ ? "P" + std::to_string(lastScorer_) + " hit" : "both hit each other";
        for (const auto& [key, p] : players_) text += "  P" + std::to_string(p.id) + "=" + std::to_string(p.score);
        logLine(text);

        bullets_.clear();
        phase_ = PHASE_ROUND_OVER;
        phaseTimer_ = ROUND_OVER_TIME;
    }

    int sock_;
    std::unordered_map<uint64_t, Player> players_;
    std::vector<Bullet> bullets_;
    Phase phase_ = PHASE_WAITING;
    float phaseTimer_ = 0;
    int round_ = 0;
    uint8_t lastScorer_ = 0;
    uint32_t tick_ = 0;
};

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : DEFAULT_PORT;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    GameServer game(sock);

    const auto tickDuration = std::chrono::microseconds(1000000 / TICK_RATE);
    const float dt = 1.0f / TICK_RATE;
    auto nextTick = Clock::now() + tickDuration;

    printf("Server (v4, shooter) started on UDP port %u, %d ticks/s\n", port, TICK_RATE);
    fflush(stdout);

    while (lastSignal == 0) {
        // 1. Ждём пакеты, но не дольше, чем до следующего тика
        int timeoutMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                            nextTick - Clock::now()).count();
        if (timeoutMs < 0) timeoutMs = 0;

        pollfd pfd{sock, POLLIN, 0};
        if (poll(&pfd, 1, timeoutMs) < 0 && errno != EINTR) { perror("poll"); break; }

        // 2. Забираем ВСЕ пришедшие пакеты (MSG_DONTWAIT — не ждать, если очередь пуста)
        while (true) {
            uint8_t buf[512];
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, (sockaddr*)&from, &fromLen);
            if (n < 0) break;   // EAGAIN — очередь пуста
            game.handlePacket(buf, (size_t)n, from);
        }

        // 3. Пора тикать: обновить мир и разослать состояние
        if (Clock::now() >= nextTick) {
            game.update(dt);
            game.broadcastState();
            nextTick += tickDuration;

            // если сильно отстали (например, машина "подвисла") — не пытаемся догонять рывком
            if (Clock::now() - nextTick > std::chrono::milliseconds(250)) nextTick = Clock::now();
        }
    }

    printf("Shutting down\n");
    close(sock);
    return 0;
}
