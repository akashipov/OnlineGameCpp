// Вариант 5 (v5_prediction): сервер стрелялки с поддержкой client-side prediction.
//
// Отличия от v4:
//   - каждый присланный INPUT кладётся в очередь игрока и применяется РОВНО ОДИН РАЗ
//     функцией applyMovement() из protocol.h с шагом INPUT_DT — точно так же, как это
//     делает клиент у себя для предсказания;
//   - в состоянии для каждого игрока передаётся lastInputSeq — номер последнего
//     применённого INPUT. По нему клиент понимает, какие свои вводы сервер уже учёл;
//   - "бюджет вводов" копится по реальному времени: пачку опоздавших вводов сервер
//     применяет сразу, но в среднем быстрее реального времени двигаться нельзя (защита от speedhack).
//
// Сервер ничего не рисует — только считает мир и рассылает его.
//
// Запуск:  ./v5_server [port]      (по умолчанию 7777)

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
#include <deque>
#include <string>
#include <vector>
#include <unordered_map>

#include "../common/protocol.h"
#include "../common/jitter.h"

using Clock = std::chrono::steady_clock;

static volatile sig_atomic_t lastSignal = 0;
void onSignal(int sig) { lastSignal = sig; }

struct Player {
    uint8_t id = 0;                  // 1 или 2
    sockaddr_in addr{};
    float x = 0, y = 0, aim = 0;
    uint8_t score = 0;
    bool alive = true;
    int fireCooldown = 0;            // сколько ВВОДОВ ждать до следующего выстрела (см. tryFire)

    std::deque<InputMsg> inputs;     // пришедшие, но ещё не применённые вводы
    uint32_t lastReceivedSeq = 0;    // самый новый пришедший seq (старые/дубли выбрасываем)
    uint32_t lastProcessedSeq = 0;   // последний применённый seq — уходит клиенту в состоянии
    float inputTokens = 0;           // сколько вводов можно применить (копится по реальному времени)
    bool hasShot = false;            // в одном из применённых за тик вводов был выстрел
    Shot shot{};                     // откуда и куда (посчитано на ТОМ САМОМ вводе)
    JitterMeter arrival;             // насколько неровно приходят вводы этого игрока
    int bufferTicks = 1;             // на сколько тиков придерживаем его вводы (для HUD/лога)

    Clock::time_point lastSeen;
};

struct Bullet {
    float x, y, vx, vy;
    float life;
    uint8_t owner;
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
                send(from, encodeWelcome(it->second.id));
                return;
            }
            if ((int)players_.size() >= MAX_PLAYERS) {
                send(from, encodeFull());
                return;
            }
            addPlayer(key, from);
            return;
        }

        if (it == players_.end()) return;
        Player& p = it->second;
        p.lastSeen = Clock::now();

        if (type == MSG_INPUT) {
            std::vector<InputMsg> received;
            if (!decodeInput(r, received)) return;
            // в пакете несколько последних вводов; берём только те, которых ещё не было
            for (const InputMsg& in : received) {
                if (in.seq <= p.lastReceivedSeq) continue;   // уже получали (дубль) или старый пакет
                if (!std::isfinite(in.aim)) return;
                p.lastReceivedSeq = in.seq;
                p.arrival.add((double)in.seq - nowTicks());
                // очередь ограничена: при переполнении выбрасываем самый СТАРЫЙ ввод,
                // иначе задержка управления "застрянет" на максимуме
                if ((int)p.inputs.size() >= MAX_QUEUED_INPUTS) p.inputs.pop_front();
                p.inputs.push_back(in);
            }
        } else if (type == MSG_LEAVE) {
            removePlayer(key, "left the game");
        }
    }

    // ---------- Один тик игры ----------

    void update(float dt) {
        tick_++;
        dropSilentPlayers();

        // сколько РЕАЛЬНОГО времени прошло с прошлого тика (если сервер подвис — больше dt)
        auto now = Clock::now();
        float elapsed = lastUpdate_ == Clock::time_point{}
            ? dt
            : std::chrono::duration<float>(now - lastUpdate_).count();
        lastUpdate_ = now;

        processInputs(elapsed);   // движение — здесь, ровно по присланным вводам

        switch (phase_) {
            case PHASE_WAITING:
                break;

            case PHASE_COUNTDOWN:
                phaseTimer_ -= dt;
                if (phaseTimer_ <= 0) phase_ = PHASE_PLAYING;
                break;

            case PHASE_PLAYING:
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

        for (auto& [key, p] : players_) p.hasShot = false;
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
            PlayerState ps;
            ps.id = p.id;
            ps.x = p.x;
            ps.y = p.y;
            ps.aim = p.aim;
            ps.score = p.score;
            ps.alive = p.alive ? 1 : 0;
            ps.lastInputSeq = p.lastProcessedSeq;
            s.players.push_back(ps);
        }
        for (const auto& b : bullets_) {
            s.bullets.push_back({b.x, b.y, b.vx, b.vy, b.owner});
        }

        Writer w = encodeState(s);
        for (const auto& [key, p] : players_) send(p.addr, w);
    }

private:
    void send(const sockaddr_in& to, const Writer& w) {
        sendto(sock_, w.data(), w.size(), 0, (const sockaddr*)&to, sizeof(to));
    }

    // ---------- Вводы ----------

    // БУФЕР ВВОДОВ (jitter buffer).
    // Вводы игрока приходят неровно (Wi-Fi: то пауза 100 мс, то пачка). Если применять каждый
    // сразу, игрок на сервере то стоит, то догоняет рывком — и у соперника он дёргается.
    // Поэтому ввод №k применяем не когда он пришёл, а по расписанию:
    //     (среднее время прихода ввода №k) + bufferTicks
    // Ввод, опоздавший меньше чем на bufferTicks, успевает к своему времени — движение ровное.
    // bufferTicks подбирается под сеть игрока: ровная сеть → 1 тик, Wi-Fi скачет → больше.
    // Сам игрок задержки не замечает (у него prediction), а соперник видит его плавно.
    void processInputs(float elapsed) {
        double now = nowTicks();
        for (auto& entry : players_) {
            Player& p = entry.second;
            if (!p.arrival.ready()) continue;
            p.bufferTicks = std::clamp((int)std::ceil(2.0 * p.arrival.jitterTicks), 1, MAX_INPUT_BUFFER);

            // когда пора применять ввод с этим номером
            auto dueAt = [&](const InputMsg& in) { return (double)in.seq - p.arrival.offset + p.bufferTicks; };

            // +1 ввод за каждые 1/60 с реального времени, копится до MAX_INPUT_BURST (защита от speedhack)
            p.inputTokens = std::min(p.inputTokens + elapsed * TICK_RATE, (float)MAX_INPUT_BURST);

            int appliedThisTick = 0;
            while (!p.inputs.empty() && dueAt(p.inputs.front()) <= now &&
                   p.inputTokens >= 1.0f && appliedThisTick < MAX_INPUTS_PER_TICK) {
                appliedThisTick++;
                InputMsg in = p.inputs.front();
                p.inputs.pop_front();
                p.inputTokens -= 1.0f;

                p.lastProcessedSeq = in.seq;
                p.aim = in.aim;
                // ТА ЖЕ функция, что у клиента в предсказании
                if (canMove(phase_, p.alive)) applyMovement(p.x, p.y, in.keys, INPUT_DT);
                // выстрел — на этом же вводе, из позиции и с прицелом этого ввода (как у клиента)
                if (tryFire(p.fireCooldown, in.keys, canShoot(phase_, p.alive))) {
                    p.shot = makeShot(p.x, p.y, p.aim);
                    p.hasShot = true;
                }
            }

            // Вводы, которым уже пора, но на них нет бюджета, — хвост, сам он не уйдёт. Выбрасываем старые.
            int overdue = 0;
            for (const auto& in : p.inputs) if (dueAt(in) <= now) overdue++;
            int dropped = 0;
            while (overdue > (int)p.inputTokens + MAX_INPUT_BACKLOG) {
                p.lastProcessedSeq = p.inputs.front().seq;   // клиент тоже их забудет
                p.inputs.pop_front();
                overdue--;
                dropped++;
            }
            if (dropped > 0)
                logLine("Player " + std::to_string(p.id) + ": dropped " + std::to_string(dropped) +
                        " late inputs (~" + std::to_string(dropped * 1000 / TICK_RATE) + " ms backlog)");
        }
    }

    // текущее время сервера в тиках (дробное) — для расписания вводов
    double nowTicks() const {
        return std::chrono::duration<double>(Clock::now() - startTime_).count() * TICK_RATE;
    }

    // ---------- Игроки ----------

    void addPlayer(uint64_t key, const sockaddr_in& from) {
        uint8_t id = 1;
        for (const auto& [k, p] : players_) if (p.id == 1) id = 2;

        Player p;
        p.arrival.decay = 0.01;   // буфер вводов сжимается за ~2 с после разовой заминки
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
        if (phase_ != PHASE_WAITING) endGame();   // ушёл посреди игры — оставшийся побеждает
    }

    void dropSilentPlayers() {
        auto now = Clock::now();
        std::vector<uint64_t> silent;
        for (const auto& [key, p] : players_) {
            if (now - p.lastSeen > std::chrono::seconds(5)) silent.push_back(key);
        }
        for (uint64_t key : silent) removePlayer(key, "timed out");
    }

    void spawn(Player& p) {
        p.y = MAP_SIZE / 2;
        if (p.id == 1) { p.x = 150;            p.aim = 0; }
        else           { p.x = MAP_SIZE - 150; p.aim = (float)M_PI; }
        p.alive = true;
        p.fireCooldown = 0;
        p.hasShot = false;
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
            else if (p.score == best) { g.winnerId = 0; }
        }

        std::string text = "GAME OVER:";
        for (const auto& s : g.scores) text += " P" + std::to_string(s.id) + "=" + std::to_string(s.score);
        text += g.winnerId ? " → P" + std::to_string(g.winnerId) + " wins" : " → draw";
        logLine(text);

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

    // ---------- Пули ----------

    void shoot() {
        for (auto& [key, p] : players_) {
            if (!p.alive || !p.hasShot) continue;
            if ((int)bullets_.size() >= MAX_BULLETS) return;
            // решение "стрелять или нет" и точка вылета уже посчитаны в processInputs (tryFire)
            bullets_.push_back({p.shot.x, p.shot.y, p.shot.vx, p.shot.vy, BULLET_LIFETIME, p.id});
        }
    }

    void moveBullets(float dt) {
        for (auto& b : bullets_) {
            b.x += b.vx * dt;
            b.y += b.vy * dt;
            b.life -= dt;
        }
        bullets_.erase(std::remove_if(bullets_.begin(), bullets_.end(), [](const Bullet& b) {
            return b.life <= 0 || b.x < 0 || b.y < 0 || b.x > MAP_SIZE || b.y > MAP_SIZE;
        }), bullets_.end());
    }

    void checkHits() {
        const float hitDist = PLAYER_RADIUS + BULLET_RADIUS;
        std::vector<uint8_t> scorers;

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
    Clock::time_point lastUpdate_{};
    Clock::time_point startTime_ = Clock::now();
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

    printf("Server (v5, prediction) started on UDP port %u, %d ticks/s\n", port, TICK_RATE);
    fflush(stdout);

    while (lastSignal == 0) {
        int timeoutMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                            nextTick - Clock::now()).count();
        if (timeoutMs < 0) timeoutMs = 0;

        pollfd pfd{sock, POLLIN, 0};
        if (poll(&pfd, 1, timeoutMs) < 0 && errno != EINTR) { perror("poll"); break; }

        while (true) {
            uint8_t buf[512];
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, (sockaddr*)&from, &fromLen);
            if (n < 0) break;
            game.handlePacket(buf, (size_t)n, from);
        }

        if (Clock::now() >= nextTick) {
            game.update(dt);
            game.broadcastState();
            nextTick += tickDuration;
            if (Clock::now() - nextTick > std::chrono::milliseconds(250)) nextTick = Clock::now();
        }
    }

    printf("Shutting down\n");
    close(sock);
    return 0;
}
