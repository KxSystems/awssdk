// System headers first; the project headers below pull in k.h, which must come last.
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "event_channel.h"


namespace awssdk {

namespace {

#ifdef _WIN32
typedef SOCKET Socket;
void close_socket(Socket s) { closesocket(s); }
#else
typedef int Socket;
void close_socket(Socket s) { close(s); }
#endif

// The work waiting for q's main thread. A socket pair wakes q's event loop:
// writing a byte to one end makes q call on_readable for the other.
//
// At most one wake-up byte is ever unread, because one is written only when
// none is pending, so a writer can never find the socket full and block. A
// blocking writer would be a deadlock risk: shutDown waits on q's main thread
// for transfer threads that would be waiting for q to read.
//
// Never destroyed: SDK threads, and the retirement helper, can still post while
// the process exits, after ordinary statics are gone.
std::mutex & queue_mutex() {
    static auto * mutex = new std::mutex();
    return *mutex;
}
std::deque<std::function<void()>> & queue() {
    static auto * work = new std::deque<std::function<void()>>();
    return *work;
}
bool wakeup_pending = false;

Socket read_end;
Socket write_end;
bool attached = false;

#ifdef _WIN32
// Windows has no socketpair, so two sockets are connected over loopback. The
// accepted connection is checked against the one just made, so a connection
// from anywhere else that reached the listener first is not mistaken for it.
bool make_pair(Socket & reader, Socket & writer) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { return false; }

    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { return false; }

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int length = sizeof(address);

    Socket connecting = INVALID_SOCKET;
    Socket accepted = INVALID_SOCKET;
    bool ok = bind(listener, (sockaddr *)&address, sizeof(address)) == 0
        && listen(listener, 1) == 0
        && getsockname(listener, (sockaddr *)&address, &length) == 0
        && (connecting = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) != INVALID_SOCKET
        && connect(connecting, (sockaddr *)&address, sizeof(address)) == 0
        && (accepted = accept(listener, nullptr, nullptr)) != INVALID_SOCKET;
    closesocket(listener);

    if (ok) {
        sockaddr_in mine;
        sockaddr_in peer;
        int mine_length = sizeof(mine);
        int peer_length = sizeof(peer);
        ok = getsockname(connecting, (sockaddr *)&mine, &mine_length) == 0
            && getpeername(accepted, (sockaddr *)&peer, &peer_length) == 0
            && mine.sin_port == peer.sin_port
            && mine.sin_addr.s_addr == peer.sin_addr.s_addr;
    }
    if (!ok) {
        if (connecting != INVALID_SOCKET) { closesocket(connecting); }
        if (accepted != INVALID_SOCKET) { closesocket(accepted); }
        return false;
    }

    u_long nonblocking = 1;
    ioctlsocket(connecting, FIONBIO, &nonblocking);
    reader = accepted;
    writer = connecting;
    return true;
}
#else
bool make_pair(Socket & reader, Socket & writer) {
    Socket ends[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) { return false; }
    fcntl(ends[1], F_SETFL, fcntl(ends[1], F_GETFL) | O_NONBLOCK);
    reader = ends[0];
    writer = ends[1];
    return true;
}
#endif

// Called by q's event loop, on the main thread, when a wake-up byte arrives.
K on_readable(I) {
    // The byte's value means nothing. sd1 made this end non-blocking, so the
    // loop stops once nothing is left to read.
    char bytes[64];
    while (recv(read_end, bytes, sizeof(bytes), 0) > 0) {}

    // Taken as a batch before any of it runs, so that work which queues more
    // work, or calls back into the module, does not run against a queue that
    // is being consumed. Anything queued from now on writes a new byte.
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        batch.swap(queue());
        wakeup_pending = false;
    }
    for (std::function<void()> & fn : batch) {
        try {
            fn();
        } catch (const std::exception & exc) {
            std::fprintf(stderr, "awssdk: %s\n", exc.what());
        } catch (...) {
            std::fprintf(stderr, "awssdk: unknown error delivering an event\n");
        }
    }
    return (K)0;
}

}  // namespace

void post_to_main(std::function<void()> fn) {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        queue().push_back(std::move(fn));
        if (!wakeup_pending) {
            wakeup_pending = true;
            wake = true;
        }
    }
    if (wake) {
        const char byte = 0;
#ifdef MSG_NOSIGNAL
        send(write_end, &byte, 1, MSG_NOSIGNAL);
#else
        send(write_end, &byte, 1, 0);
#endif
    }
}

void attach_event_channel() {
    if (attached) { return; }
    Socket reader;
    Socket writer;
    if (!make_pair(reader, writer)) { throw std::runtime_error("eventChannel"); }
    read_end = reader;
    write_end = writer;

    // A negative descriptor tells sd1 to make the socket non-blocking. Written
    // as 0 - d: a Windows SOCKET is unsigned. sd1 returns the descriptor as an
    // integer, or NULL having closed it; the channel is then left unattached,
    // so that the next async call tries again.
    K result = sd1(0 - (I)read_end, on_readable);
    if (result == nullptr) {
        close_socket(write_end);
        throw std::runtime_error("eventChannel");
    }
    r0(result);
    attached = true;
}

}
