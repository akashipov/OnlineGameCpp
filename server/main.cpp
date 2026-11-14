#include <sys/socket.h>   // socket, bind, sendto, recvfrom
#include <netinet/in.h>   // sockaddr_in, INADDR_ANY, htons
#include <arpa/inet.h>    // inet_ntop (turn an IP into text)
#include <unistd.h>       // close
#include <cstdio>         // perror
#include <csignal>   // sigaction, SIGINT
#include <cerrno>     // errno, EINTR
#include <string.h>
#include <string>
#include <map>
#include <unordered_map>
#include <iostream>

static volatile sig_atomic_t lastSignal = 0;

void onSignal(int sig) {
    lastSignal = sig;     // в обработчике можно делать только очень простые вещи
}

struct Player {
    sockaddr_in* client;
    int32_t score;

    Player() {
    }

    Player(sockaddr_in* client_, int32_t score_)
        : client(client_), score(score_)     // список инициализации членов
    {
        // тело конструктора: дополнительная логика, если нужна
    }
};

std::string get_id_client(sockaddr_in* client) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client->sin_addr, ip, sizeof(ip));
    return ip + std::to_string(ntohs(client->sin_port));
}

struct Game {
    int sock;
    Player players[2];
    size_t count = 0;

    Game(int sock_) : sock(sock_) {
    }

    void add(Player player) {
        if (count == 1) {
            if (get_id_client(players[0].client) == get_id_client(player.client)) {
                std::string answer = "impossible to play again, wait answer from another player";
                sendto(sock, &answer, answer.size(), 0, (sockaddr*)player.client, sizeof(*player.client));
                return;
            }
        }

        players[count] = player;
        count++;

        if (count == 2) {
            int winner = 0;
            if (players[1].score > players[0].score) {
                winner = 1;
            } else if (players[1].score == players[0].score) {
                winner = -1;
            }


            if (winner == -1) {
                std::string answer = "ничья";
                sendto(sock, &answer, answer.size(), 0, (sockaddr*)players[0].client, sizeof(*players[0].client));
                sendto(sock, &answer, answer.size(), 0, (sockaddr*)players[1].client, sizeof(*players[1].client));
            } else {
                std::string answer = "вы выиграли";
                sendto(sock, &answer, answer.size(), 0, (sockaddr*)players[winner].client, sizeof(*players[winner].client));
                answer = "вы проиграли";
                winner ^= 1;
                sendto(sock, &answer, answer.size(), 0, (sockaddr*)players[winner].client, sizeof(*players[winner].client));
            }

            count = 0;
            return;
        }
    }
};

int main() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                    // важно: без SA_RESTART (см. ниже)
    sigaction(SIGINT, &sa, nullptr);    // Ctrl+C
    sigaction(SIGTERM, &sa, nullptr);   // команда kill

    sockaddr_in addr{};
    addr.sin_family = AF_INET;                 // IPv4
    addr.sin_port = htons(7777);               // port 7777
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    printf("Server started on port 7777\n");

    Game game(sock);

    while (lastSignal == 0) {
        uint32_t replyNet;                      // сюда придут данные
        sockaddr_in client{};                // сюда ОС запишет адрес отправителя
        socklen_t clientLen = sizeof(client);

        // Ждём пакет (программа "засыпает" здесь, пока что-то не придёт)
        ssize_t n = recvfrom(sock, &replyNet, sizeof(replyNet), 0,
                             (sockaddr*)&client, &clientLen);
        if (n < 0) {
            if (errno == EINTR) {
                printf("recvfrom interrupted by signal\n");
                continue;            // while проверит running == false и выйдет
            }
            perror("recvfrom");
            continue;
        }
        int32_t reply = (int32_t)ntohl(replyNet);

        // Кто прислал
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        game.add(Player(&client, reply));
        printf("Got from %s:%d: %d\n", ip, ntohs(client.sin_port), reply);

        
        sendto(sock, &answer, answer.size(), 0, (sockaddr*)&client, clientLen);
    }

    int sig = lastSignal;
    printf("Stopped by signal %d (%s)\n", sig, strsignal(sig));   // нужен <cstring>
    close(sock);
    return 0;
}
