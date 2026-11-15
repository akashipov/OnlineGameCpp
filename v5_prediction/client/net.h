#pragma once
// UDP-клиент с одинаковым интерфейсом на Mac/Linux и на Windows.
//
// На Mac/Linux сокеты — это BSD sockets (sys/socket.h), на Windows — Winsock (winsock2.h).
// Функции почти те же (socket, sendto, recvfrom), но есть отличия:
//   - на Windows перед работой с сетью нужно вызвать WSAStartup();
//   - закрывается сокет через closesocket(), а не close();
//   - неблокирующий режим — ioctlsocket(), а не fcntl().
// Всё это спрятано в net.cpp.
//
// Почему отдельный .cpp, а не просто #ifdef в main.cpp: windows.h (его тянет winsock2.h)
// и raylib.h объявляют одинаковые имена (Rectangle, DrawText, CloseWindow...),
// и в одном файле они не компилируются. А так main.cpp вообще не видит windows.h.

#include <cstddef>
#include <cstdint>

class UdpClient {
public:
    // Создаёт сокет и находит адрес сервера (host может быть IP или именем)
    bool open(const char* host, uint16_t port);

    // Отправить пакет серверу
    bool send(const void* data, size_t size);

    // Забрать один пришедший пакет. Не ждёт: если пакетов нет — сразу возвращает 0.
    // Иначе — размер пакета в байтах.
    int receive(void* buf, size_t capacity);

    void close();
    ~UdpClient() { close(); }

private:
    intptr_t sock_ = -1;                     // на Windows SOCKET — это число размером с указатель
    alignas(8) unsigned char server_[16]{};  // здесь лежит sockaddr_in сервера (16 байт на всех ОС)
    [[maybe_unused]] bool started_ = false;  // вызывали ли WSAStartup (нужно только на Windows)
};
