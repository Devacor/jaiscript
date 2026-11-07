#pragma once
// The socket primitives the runner's NetChannel/NetServer and the jai_spider host share: one spelling of a
// non-blocking TCP socket on Winsock and on POSIX.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <cerrno>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace jai_runner {

#ifdef _WIN32
using net_socket = SOCKET;
constexpr net_socket net_bad_socket = INVALID_SOCKET;
inline bool net_ready() {
	static const bool ready = [] { WSADATA wsa; return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0; }();
	return ready;
}
inline bool net_would_block() {
	int e = ::WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
inline void net_close_socket(net_socket s) { ::closesocket(s); }
inline void net_set_nonblocking(net_socket s) {
	u_long mode = 1;
	::ioctlsocket(s, FIONBIO, &mode);
}
// Windows' SO_REUSEADDR lets a second listener take a port that is in use; exclusive use makes that bind fail
inline void net_listen_options(net_socket s) {
	BOOL yes = TRUE;
	::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
}
#else
using net_socket = int;
constexpr net_socket net_bad_socket = -1;
inline bool net_ready() { return true; }
inline bool net_would_block() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
inline void net_close_socket(net_socket s) { ::close(s); }
inline void net_set_nonblocking(net_socket s) { ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
inline void net_listen_options(net_socket s) {
	int yes = 1;
	::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
}
#endif

} // namespace jai_runner
