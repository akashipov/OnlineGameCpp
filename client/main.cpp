#include <sys/socket.h>   // socket, sendto, recvfrom, setsockopt
#include <netinet/in.h>   // sockaddr_in, htons
#include <arpa/inet.h>    // inet_pton
#include <unistd.h>       // close
#include <sys/time.h>     // timeval
#include <cstdio>         // printf, fgets, perror
#include <cstring>        // strlen, strcspn
#include <cerrno>         // errno, EAGAIN
#include <chrono>         // измерение пинга
#include <charconv>       // std::from_chars
#include <iostream>       // std::cout, std::endl
#include <cstdint>

int main() {
    // 1. Создаём UDP-сокет (bind не нужен: ОС сама выберет свободный порт)
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // 2. Таймаут на приём: если ответ не пришёл за 1 секунду, не ждём вечно
    timeval tv{};
    tv.tv_sec = 5;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 3. Адрес сервера
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(7777);
    if (inet_pton(AF_INET, "127.0.0.1", &server.sin_addr) != 1) {
        printf("Bad server address\n");
        return 1;
    }

    printf("Type a message and press Enter (Ctrl+D to quit)\n");

    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {      // читаем строку с клавиатуры
        line[strcspn(line, "\n")] = '\0';          // убираем перевод строки

        int32_t value;
        auto [ptr, ec] = std::from_chars(line, line + strlen(line), value);

        if (ec == std::errc() && *ptr == '\0') {
            // успех, value = 42
        } else if (ec == std::errc::result_out_of_range) {
            std::cout << "не влазит в int32, попробуйте еще раз..." << std::endl;
            continue;
        } else {
            std::cout << "не число, попробуйте еще раз..." << std::endl;
            continue;
        }

        auto t0 = std::chrono::steady_clock::now();

        uint32_t net = htonl((uint32_t)value);
        // 4. Отправляем серверу
        if (sendto(sock, &net, sizeof(net), 0, (sockaddr*)&server, sizeof(server)) < 0) {
            perror("sendto");
            continue;
        }

        // 5. Ждём ответ
        char buf[1024];
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN) {
                printf("No reply (timeout) - packet lost or server is down\n");
            } else {
                perror("recvfrom");
            }
            continue;
        }
        buf[n] = '\0';

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        printf("Reply: %s (ping %.3f ms)\n", buf, ms);
    }

    close(sock);
    return 0;
}
