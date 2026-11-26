// winsock_offline: Winsock for a mapped title, which reaches loopback only.
//
// The retail executable imports 25 functions from ws2_32.dll, 23 by ordinal plus getaddrinfo and freeaddrinfo
// (docs/runtime_imports.md, RT-5), and they were bound to the host's: a --run could open real sockets, against the
// "nothing online" rule. Ported from VIctory victory-hle src/modules/net.rs (GPL-3.0-or-later), "a console without a
// network cable": every way out of Winsock a title can import (by name, or by the classic ordinals below) is vetted
// here, and the rest of ws2_32.dll stays the host's (registry.cpp keeps it a host module, as advapi32.dll).
//   - connect / WSAConnect, the ConnectEx and WSASendMsg extensions WSAIoctl hands out, and datagram sends (sendto,
//     WSASendTo) to anything but 127.0.0.0/8 and ::1 (IPv4-mapped too) fail with WSAENETUNREACH; nothing is sent;
//   - forward lookups (getaddrinfo, GetAddrInfoW, gethostbyname, WSAConnectByNameA/W) of anything but "localhost" and
//     IP literals fail with WSAHOST_NOT_FOUND, so no DNS query leaves the machine;
//   - reverse lookups (gethostbyaddr, getnameinfo) of other addresses fail or come back numeric (NI_NAMEREQD fails),
//     as on a console with no network;
//   - a bind to all interfaces (INADDR_ANY, in6addr_any) is narrowed to loopback, so nothing on the network can
//     connect in.
// Each refused destination is logged once ("[rt] winsock: offline, refused ..."), never with a payload. Only images
// mapped by the loader see these: they are bound through the registry, the runtime's own code calls the host directly.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "shims.h"

namespace rt::shims::winsock_offline {
namespace {

void refused(const char* what, const std::string& destination) {
    static auto* m = new std::mutex;
    static auto* seen = new std::set<std::string>;
    std::lock_guard lock(*m);
    if (seen->insert(std::string(what) + " " + destination).second)
        std::fprintf(stderr, "[rt] winsock: offline, refused %s %s\n", what, destination.c_str());
}

bool loopback_v4(const in_addr& a) { return (ntohl(a.s_addr) >> 24) == 127; }
bool loopback_v6(const in6_addr& a) {
    static const unsigned char kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(a.s6_addr, kLoopback, 16) == 0) return true;
    // ::ffff:127.x.y.z, an IPv4 loopback address in IPv6 form
    static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::memcmp(a.s6_addr, kMapped, 12) == 0 && a.s6_addr[12] == 127;
}

// Whether a socket address is a loopback one. Families other than IPv4 and IPv6 (a Unix socket, say) are local.
bool loopback(const sockaddr* addr, int len) {
    if (!addr || len < static_cast<int>(sizeof(sockaddr))) return true;   // the host rejects it as invalid
    if (addr->sa_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in)))
        return loopback_v4(reinterpret_cast<const sockaddr_in*>(addr)->sin_addr);
    if (addr->sa_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6)))
        return loopback_v6(reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr);
    return addr->sa_family != AF_INET && addr->sa_family != AF_INET6;
}

std::string describe(const sockaddr* addr, int len) {
    char text[INET6_ADDRSTRLEN] = "?";
    if (addr && addr->sa_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in))) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
        inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
        return std::string(text) + ":" + std::to_string(ntohs(in->sin_port));
    }
    if (addr && addr->sa_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6))) {
        const auto* in = reinterpret_cast<const sockaddr_in6*>(addr);
        inet_ntop(AF_INET6, &in->sin6_addr, text, sizeof(text));
        return std::string("[") + text + "]:" + std::to_string(ntohs(in->sin6_port));
    }
    return text;
}

// Refuses a destination that is not loopback: true when refused (WSAENETUNREACH set).
bool refuse_destination(const char* what, const sockaddr* addr, int len) {
    if (loopback(addr, len)) return false;
    refused(what, describe(addr, len));
    WSASetLastError(WSAENETUNREACH);
    return true;
}

// "localhost" (any case, optionally with a trailing '.') or an IP literal: names that resolve without DNS.
bool local_name(const std::string& name) {
    std::string bare = name;
    if (!bare.empty() && bare.back() == '.') bare.pop_back();
    if (_stricmp(bare.c_str(), "localhost") == 0) return true;
    if (bare.size() > 2 && bare.front() == '[' && bare.back() == ']') bare = bare.substr(1, bare.size() - 2);
    in_addr v4{};
    in6_addr v6{};
    return inet_pton(AF_INET, bare.c_str(), &v4) == 1 || inet_pton(AF_INET6, bare.c_str(), &v6) == 1;
}

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; ++w) s += (*w < 0x80) ? static_cast<char>(*w) : '?';
    return s;
}

// ---- connects and sends ----

int WSAAPI shim_connect(SOCKET s, const sockaddr* name, int namelen) {
    if (refuse_destination("connect to", name, namelen)) return SOCKET_ERROR;
    return connect(s, name, namelen);
}

int WSAAPI shim_wsa_connect(SOCKET s, const sockaddr* name, int namelen, LPWSABUF caller, LPWSABUF callee, LPQOS sqos,
                            LPQOS gqos) {
    if (refuse_destination("connect to", name, namelen)) return SOCKET_ERROR;
    return WSAConnect(s, name, namelen, caller, callee, sqos, gqos);
}

int WSAAPI shim_sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    // A null destination is the connected peer, which connect() kept on loopback.
    if (to && refuse_destination("datagram to", to, tolen)) return SOCKET_ERROR;
    return sendto(s, buf, len, flags, to, tolen);
}

int WSAAPI shim_wsa_send_to(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to,
                            int tolen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    if (to && refuse_destination("datagram to", to, tolen)) return SOCKET_ERROR;
    return WSASendTo(s, buffers, count, sent, flags, to, tolen, overlapped, completion);
}

// ---- WSAIoctl's extension functions ----

std::atomic<LPFN_CONNECTEX> g_host_connect_ex{nullptr};
std::atomic<LPFN_WSASENDMSG> g_host_send_msg{nullptr};

BOOL PASCAL shim_connect_ex(SOCKET s, const sockaddr* name, int namelen, PVOID send, DWORD send_length, LPDWORD sent,
                            LPOVERLAPPED overlapped) {
    if (refuse_destination("connect to", name, namelen)) return FALSE;
    const LPFN_CONNECTEX host = g_host_connect_ex.load();
    if (!host) {
        WSASetLastError(WSAEINVAL);
        return FALSE;
    }
    return host(s, name, namelen, send, send_length, sent, overlapped);
}

INT WSAAPI shim_send_msg(SOCKET s, LPWSAMSG msg, DWORD flags, LPDWORD sent, LPWSAOVERLAPPED overlapped,
                         LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    if (msg && msg->name && refuse_destination("datagram to", msg->name, msg->namelen)) return SOCKET_ERROR;
    const LPFN_WSASENDMSG host = g_host_send_msg.load();
    if (!host) {
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
    return host(s, msg, flags, sent, overlapped, completion);
}

// WSAIoctl is the host's, except that the ConnectEx and WSASendMsg pointers it hands out are ours, which vet the
// destination and then call the host's (one provider per process is assumed: the one the title asked through).
int WSAAPI shim_wsa_ioctl(SOCKET s, DWORD code, LPVOID in, DWORD in_size, LPVOID out, DWORD out_size, LPDWORD returned,
                          LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    const int result = WSAIoctl(s, code, in, in_size, out, out_size, returned, overlapped, completion);
    if (result != 0 || code != SIO_GET_EXTENSION_FUNCTION_POINTER || !in || in_size < sizeof(GUID) || !out ||
        out_size < sizeof(void*))
        return result;
    const GUID connect_ex = WSAID_CONNECTEX, send_msg = WSAID_WSASENDMSG;
    if (std::memcmp(in, &connect_ex, sizeof(GUID)) == 0) {
        g_host_connect_ex.store(*static_cast<LPFN_CONNECTEX*>(out));
        *static_cast<LPFN_CONNECTEX*>(out) = &shim_connect_ex;
    } else if (std::memcmp(in, &send_msg, sizeof(GUID)) == 0) {
        g_host_send_msg.store(*static_cast<LPFN_WSASENDMSG*>(out));
        *static_cast<LPFN_WSASENDMSG*>(out) = &shim_send_msg;
    }
    return result;
}

// ---- binds ----

int WSAAPI shim_bind(SOCKET s, const sockaddr* addr, int len) {
    if (addr && addr->sa_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in)) &&
        reinterpret_cast<const sockaddr_in*>(addr)->sin_addr.s_addr == htonl(INADDR_ANY)) {
        sockaddr_in narrowed = *reinterpret_cast<const sockaddr_in*>(addr);
        narrowed.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return bind(s, reinterpret_cast<const sockaddr*>(&narrowed), sizeof(narrowed));
    }
    if (addr && addr->sa_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6)) &&
        IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr)) {
        sockaddr_in6 narrowed = *reinterpret_cast<const sockaddr_in6*>(addr);
        narrowed.sin6_addr = in6addr_loopback;
        return bind(s, reinterpret_cast<const sockaddr*>(&narrowed), sizeof(narrowed));
    }
    return bind(s, addr, len);
}

// ---- forward lookups ----

INT WSAAPI shim_getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
    if (node && !local_name(node)) {
        refused("lookup of", node);
        if (result) *result = nullptr;
        WSASetLastError(WSAHOST_NOT_FOUND);
        return WSAHOST_NOT_FOUND;   // EAI_NONAME
    }
    return getaddrinfo(node, service, hints, result);
}

INT WSAAPI shim_get_addr_info_w(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* result) {
    if (node && !local_name(narrow(node))) {
        refused("lookup of", narrow(node));
        if (result) *result = nullptr;
        WSASetLastError(WSAHOST_NOT_FOUND);
        return WSAHOST_NOT_FOUND;
    }
    return GetAddrInfoW(node, service, hints, result);
}

#pragma warning(push)
#pragma warning(disable : 4996)   // gethostbyname / gethostbyaddr: deprecated, still what the ordinals name
hostent* WSAAPI shim_gethostbyname(const char* name) {
    if (name && !local_name(name)) {
        refused("lookup of", name);
        WSASetLastError(WSAHOST_NOT_FOUND);
        return nullptr;
    }
    return gethostbyname(name);
}

// ---- reverse lookups ----

hostent* WSAAPI shim_gethostbyaddr(const char* addr, int len, int type) {
    const bool v4 = type == AF_INET && addr && len >= 4;
    const bool v6 = type == AF_INET6 && addr && len >= 16;
    bool local = !v4 && !v6;
    if (v4) {
        in_addr a{};
        std::memcpy(&a, addr, 4);
        local = loopback_v4(a);
    } else if (v6) {
        in6_addr a{};
        std::memcpy(&a, addr, 16);
        local = loopback_v6(a);
    }
    if (!local) {
        refused("reverse lookup of", "an address");
        WSASetLastError(WSAHOST_NOT_FOUND);
        return nullptr;
    }
    return gethostbyaddr(addr, len, type);
}
#pragma warning(pop)

INT WSAAPI shim_getnameinfo(const SOCKADDR* addr, socklen_t len, PCHAR host, DWORD host_size, PCHAR serv,
                            DWORD serv_size, INT flags) {
    if (host && !loopback(addr, len)) {
        if (flags & NI_NAMEREQD) {
            WSASetLastError(WSAHOST_NOT_FOUND);
            return WSAHOST_NOT_FOUND;
        }
        flags |= NI_NUMERICHOST;   // the numeric form needs no DNS
    }
    return getnameinfo(addr, len, host, host_size, serv, serv_size, flags);
}

INT WSAAPI shim_get_name_info_w(const SOCKADDR* addr, socklen_t len, PWCHAR host, DWORD host_size, PWCHAR serv,
                                DWORD serv_size, INT flags) {
    if (host && !loopback(addr, len)) {
        if (flags & NI_NAMEREQD) {
            WSASetLastError(WSAHOST_NOT_FOUND);
            return WSAHOST_NOT_FOUND;
        }
        flags |= NI_NUMERICHOST;
    }
    return GetNameInfoW(addr, len, host, host_size, serv, serv_size, flags);
}

// ---- connect by name ----

#pragma warning(push)
#pragma warning(disable : 4996)   // WSAConnectByNameA: deprecated in favour of the W form, still exported
BOOL PASCAL shim_wsa_connect_by_name_a(SOCKET s, LPCSTR name, LPCSTR service, LPDWORD local_len, LPSOCKADDR local,
                                       LPDWORD remote_len, LPSOCKADDR remote, const timeval* timeout,
                                       LPWSAOVERLAPPED reserved) {
    if (!name || !local_name(name)) {
        refused("connect to", name ? name : "(null)");
        WSASetLastError(WSAHOST_NOT_FOUND);
        return FALSE;
    }
    return WSAConnectByNameA(s, name, service, local_len, local, remote_len, remote, timeout, reserved);
}
#pragma warning(pop)

BOOL PASCAL shim_wsa_connect_by_name_w(SOCKET s, LPWSTR name, LPWSTR service, LPDWORD local_len, LPSOCKADDR local,
                                       LPDWORD remote_len, LPSOCKADDR remote, const timeval* timeout,
                                       LPWSAOVERLAPPED reserved) {
    if (!name || !local_name(narrow(name))) {
        refused("connect to", name ? narrow(name) : "(null)");
        WSASetLastError(WSAHOST_NOT_FOUND);
        return FALSE;
    }
    return WSAConnectByNameW(s, name, service, local_len, local, remote_len, remote, timeout, reserved);
}

}  // namespace
}  // namespace rt::shims::winsock_offline

namespace {
using namespace rt::shims::winsock_offline;
#define WS(name, fn) {"ws2_32.dll", name, reinterpret_cast<void*>(&fn)}
}  // namespace

// The classic ordinals (ws2_32.dll's export table: 2 bind, 4 connect, 20 sendto, 51 gethostbyaddr, 52
// gethostbyname) are what the retail executable imports by number; the names cover everything else.
SHIM_TABLE(winsock_offline,
    WS("connect", shim_connect), WS("#4", shim_connect),
    WS("bind", shim_bind), WS("#2", shim_bind),
    WS("sendto", shim_sendto), WS("#20", shim_sendto),
    WS("gethostbyaddr", shim_gethostbyaddr), WS("#51", shim_gethostbyaddr),
    WS("gethostbyname", shim_gethostbyname), WS("#52", shim_gethostbyname),
    WS("WSAConnect", shim_wsa_connect),
    WS("WSASendTo", shim_wsa_send_to),
    WS("WSAIoctl", shim_wsa_ioctl),
    WS("getaddrinfo", shim_getaddrinfo),
    WS("GetAddrInfoW", shim_get_addr_info_w),
    WS("getnameinfo", shim_getnameinfo),
    WS("GetNameInfoW", shim_get_name_info_w),
    WS("WSAConnectByNameA", shim_wsa_connect_by_name_a),
    WS("WSAConnectByNameW", shim_wsa_connect_by_name_w),
);
