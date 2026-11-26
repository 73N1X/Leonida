// winsock_offline (shims/winsock_offline.cpp): Winsock as a mapped title sees it reaches loopback only. Every
// refused call is refused before the host is called, so nothing here leaves the machine; the sockets it opens are
// loopback ones. 192.0.2.1 is TEST-NET-1 (RFC 5737) and .invalid a reserved name (RFC 2606).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

int g_failures = 0;
void check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) ++g_failures;
}

template <class F> F get(const char* name) { return reinterpret_cast<F>(rt::shims::find("ws2_32.dll", name)); }

sockaddr_in v4(const char* ip, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

}  // namespace

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("SKIP: no Winsock\n");
        return 0;
    }
    using Connect = int(WSAAPI*)(SOCKET, const sockaddr*, int);
    using SendTo = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
    using Bind = int(WSAAPI*)(SOCKET, const sockaddr*, int);
    using GetAddrInfo = INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
    using GetHostByName = hostent*(WSAAPI*)(const char*);
    using GetNameInfo = INT(WSAAPI*)(const SOCKADDR*, socklen_t, PCHAR, DWORD, PCHAR, DWORD, INT);
    using Ioctl = int(WSAAPI*)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED,
                               LPWSAOVERLAPPED_COMPLETION_ROUTINE);
    const auto connect_ = get<Connect>("connect");
    const auto sendto_ = get<SendTo>("sendto");
    const auto bind_ = get<Bind>("bind");
    const auto getaddrinfo_ = get<GetAddrInfo>("getaddrinfo");
    const auto gethostbyname_ = get<GetHostByName>("gethostbyname");
    const auto getnameinfo_ = get<GetNameInfo>("getnameinfo");
    const auto ioctl_ = get<Ioctl>("WSAIoctl");
    check(connect_ && sendto_ && bind_ && getaddrinfo_ && gethostbyname_ && getnameinfo_ && ioctl_,
          "the vetted imports are served");
    check(get<Connect>("#4") == connect_ && get<SendTo>("#20") == sendto_ && get<Bind>("#2") == bind_ &&
              get<GetHostByName>("#52") == gethostbyname_ && rt::shims::find("ws2_32.dll", "#51"),
          "the classic ordinals the retail executable imports name the same functions");
    check(!rt::shims::owns_dll("ws2_32.dll"), "ws2_32.dll stays a host module: the rest of it is the host's");
    if (!(connect_ && sendto_ && bind_ && getaddrinfo_ && gethostbyname_ && getnameinfo_ && ioctl_)) return 1;

    // Connects and datagrams leave loopback only.
    SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in remote = v4("192.0.2.1", 80);
    check(connect_(tcp, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == SOCKET_ERROR &&
              WSAGetLastError() == WSAENETUNREACH,
          "a connect to a network address fails with WSAENETUNREACH");
    sockaddr_in6 mapped{};
    mapped.sin6_family = AF_INET6;
    mapped.sin6_port = htons(80);
    inet_pton(AF_INET6, "::ffff:192.0.2.1", &mapped.sin6_addr);
    SOCKET tcp6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    check(connect_(tcp6, reinterpret_cast<sockaddr*>(&mapped), sizeof(mapped)) == SOCKET_ERROR &&
              WSAGetLastError() == WSAENETUNREACH,
          "so does one to an IPv4-mapped IPv6 network address");
    closesocket(tcp6);

    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in any = v4("0.0.0.0", 0);
    check(bind_(udp, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0, "a bind to all interfaces succeeds");
    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    getsockname(udp, reinterpret_cast<sockaddr*>(&bound), &bound_len);
    check(bound.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "and is narrowed to loopback");
    sockaddr_in dns = v4("192.0.2.1", 53);
    check(sendto_(udp, "x", 1, 0, reinterpret_cast<sockaddr*>(&dns), sizeof(dns)) == SOCKET_ERROR &&
              WSAGetLastError() == WSAENETUNREACH,
          "a datagram to a network address fails with WSAENETUNREACH");
    sockaddr_in self{};
    int self_len = sizeof(self);
    getsockname(udp, reinterpret_cast<sockaddr*>(&self), &self_len);
    check(sendto_(udp, "x", 1, 0, reinterpret_cast<sockaddr*>(&self), sizeof(self)) == 1,
          "a datagram to a loopback address is sent");
    check(connect_(udp, reinterpret_cast<sockaddr*>(&self), sizeof(self)) == 0, "a connect to loopback succeeds");

    // ConnectEx from WSAIoctl is ours, and vets the destination too.
    GUID connect_ex_id = WSAID_CONNECTEX;
    LPFN_CONNECTEX connect_ex = nullptr;
    DWORD returned = 0;
    check(ioctl_(tcp, SIO_GET_EXTENSION_FUNCTION_POINTER, &connect_ex_id, sizeof(connect_ex_id), &connect_ex,
                 sizeof(connect_ex), &returned, nullptr, nullptr) == 0 && connect_ex,
          "WSAIoctl hands out ConnectEx");
    if (connect_ex)
        check(!connect_ex(tcp, reinterpret_cast<sockaddr*>(&remote), sizeof(remote), nullptr, 0, nullptr, nullptr) &&
                  WSAGetLastError() == WSAENETUNREACH,
              "its ConnectEx refuses a network address");

    // Lookups: localhost and literals only.
    ADDRINFOA* result = reinterpret_cast<ADDRINFOA*>(1);
    check(getaddrinfo_("example.invalid", "80", nullptr, &result) == WSAHOST_NOT_FOUND && !result,
          "getaddrinfo of a network name fails with WSAHOST_NOT_FOUND");
    result = nullptr;
    check(getaddrinfo_("127.0.0.1", "80", nullptr, &result) == 0 && result, "getaddrinfo of a literal is the host's");
    if (result) freeaddrinfo(result);
    result = nullptr;
    check(getaddrinfo_("localhost", "80", nullptr, &result) == 0 && result, "getaddrinfo of localhost is the host's");
    if (result) freeaddrinfo(result);
    check(gethostbyname_("example.invalid") == nullptr && WSAGetLastError() == WSAHOST_NOT_FOUND,
          "gethostbyname of a network name fails");
    char host[NI_MAXHOST] = {};
    check(getnameinfo_(reinterpret_cast<sockaddr*>(&remote), sizeof(remote), host, sizeof(host), nullptr, 0, 0) == 0 &&
              std::strcmp(host, "192.0.2.1") == 0,
          "getnameinfo of a network address comes back numeric");
    check(getnameinfo_(reinterpret_cast<sockaddr*>(&remote), sizeof(remote), host, sizeof(host), nullptr, 0,
                       NI_NAMEREQD) == WSAHOST_NOT_FOUND,
          "and fails when a name is required");

    closesocket(udp);
    closesocket(tcp);
    WSACleanup();
    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
