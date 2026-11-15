// Реализация UdpClient для двух систем. Какая ветка компилируется — решает
// препроцессор: компилятор под Windows сам определяет макрос _WIN32.

#include "net.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
// ---------------------------- Windows (Winsock) ----------------------------
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // Windows 7+: нужно для getaddrinfo
#endif
#define WIN32_LEAN_AND_MEAN       // не тянуть лишнее из windows.h
#include <winsock2.h>
#include <ws2tcpip.h>

using NativeSocket = SOCKET;
static const NativeSocket BAD_SOCKET = INVALID_SOCKET;

static void closeNative(NativeSocket s) { closesocket(s); }

static bool setNonBlocking(NativeSocket s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}

// Особенность Windows: если сервер не запущен, Windows получает ICMP "порт недоступен",
// и СЛЕДУЮЩИЙ recvfrom на UDP-сокете возвращает ошибку WSAECONNRESET.
// На Mac/Linux так не бывает. Отключаем это поведение, чтобы было одинаково.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
static void disableConnReset(NativeSocket s) {
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
}

#else
// ---------------------------- Mac / Linux (BSD sockets) ----------------------------
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>

using NativeSocket = int;
static const NativeSocket BAD_SOCKET = -1;

static void closeNative(NativeSocket s) { ::close(s); }

static bool setNonBlocking(NativeSocket s) {
    return fcntl(s, F_SETFL, O_NONBLOCK) == 0;
}

static void disableConnReset(NativeSocket) {}   // не нужно

#endif
// ---------------------------- Общий код ----------------------------

static_assert(sizeof(sockaddr_in) <= 16, "sockaddr_in must fit into server_");

static NativeSocket native(intptr_t s) { return (NativeSocket)s; }

bool UdpClient::open(const char* host, uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return false;
    }
    started_ = true;
#endif

    // Ищем адрес сервера (IPv4)
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    int err = getaddrinfo(host, nullptr, &hints, &res);
    if (err != 0 || res == nullptr) {
        printf("Cannot resolve %s (error %d)\n", host, err);
        return false;
    }
    sockaddr_in addr;
    memcpy(&addr, res->ai_addr, sizeof(addr));
    addr.sin_port = htons(port);
    freeaddrinfo(res);
    memcpy(server_, &addr, sizeof(addr));

    NativeSocket s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BAD_SOCKET) {
        printf("Cannot create socket\n");
        return false;
    }
    if (!setNonBlocking(s)) {
        printf("Cannot make socket non-blocking\n");
        closeNative(s);
        return false;
    }
    disableConnReset(s);
    sock_ = (intptr_t)s;
    return true;
}

bool UdpClient::send(const void* data, size_t size) {
    if (sock_ == -1) return false;
    // на Windows sendto хочет const char* и int, на Mac/Linux — const void* и size_t;
    // char* и int подходят обоим
    int sent = (int)sendto(native(sock_), (const char*)data, (int)size, 0,
                           (const sockaddr*)server_, sizeof(sockaddr_in));
    return sent == (int)size;
}

int UdpClient::receive(void* buf, size_t capacity) {
    if (sock_ == -1) return 0;
    int n = (int)recvfrom(native(sock_), (char*)buf, (int)capacity, 0, nullptr, nullptr);
    // n < 0: пакетов сейчас нет (или ошибка) — для игры это одно и то же: попробуем в следующем кадре
    return n > 0 ? n : 0;
}

void UdpClient::close() {
    if (sock_ != -1) {
        closeNative(native(sock_));
        sock_ = -1;
    }
#ifdef _WIN32
    if (started_) {
        WSACleanup();
        started_ = false;
    }
#endif
}
