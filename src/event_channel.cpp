// System headers first; the project headers below pull in k.h, which must come last.
#include <atomic>
#include <cerrno>
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
// Read by SDK threads in post_to_main, so atomic; set once, on the main thread,
// after the descriptors above.
std::atomic<bool> attached(false);

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

    // A blocking writer could deadlock against shutDown, so this is required.
    u_long nonblocking = 1;
    if (ioctlsocket(connecting, FIONBIO, &nonblocking) != 0) {
        closesocket(connecting);
        closesocket(accepted);
        return false;
    }
    reader = accepted;
    writer = connecting;
    return true;
}
#else
bool make_pair(Socket & reader, Socket & writer) {
    Socket ends[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) { return false; }
    // A blocking writer could deadlock against shutDown, so this is required.
    const int flags = fcntl(ends[1], F_GETFL);
    if (flags == -1 || fcntl(ends[1], F_SETFL, flags | O_NONBLOCK) == -1) {
        close(ends[0]);
        close(ends[1]);
        return false;
    }
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

// Writes the wake-up byte. True if q will wake: the byte was written, or the
// socket is full, which means a byte is already waiting to be read.
bool send_wakeup() {
    const char byte = 0;
    for (;;) {
#ifdef _WIN32
        if (send(write_end, &byte, 1, 0) == 1) { return true; }
        const int error = WSAGetLastError();
        if (error == WSAEINTR) { continue; }
        return error == WSAEWOULDBLOCK;
#else
#ifdef MSG_NOSIGNAL
        const ssize_t sent = send(write_end, &byte, 1, MSG_NOSIGNAL);
#else
        const ssize_t sent = send(write_end, &byte, 1, 0);
#endif
        if (sent == 1) { return true; }
        if (errno == EINTR) { continue; }
        return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    }
}

// Wakes q if there is work and no wake-up is outstanding. Before the channel is
// attached there is nothing to write to: the work waits, and attaching wakes
// q for it.
void wake_if_needed() {
    std::lock_guard<std::mutex> lock(queue_mutex());
    if (!attached || wakeup_pending || queue().empty()) { return; }
    // Sent with the lock held, which is safe because the socket never blocks,
    // so that no other thread can see a wake-up claimed that was never sent:
    // the flag is set only once the byte is on its way. If it is lost, the
    // flag stays clear and the next post tries again.
    if (send_wakeup()) {
        wakeup_pending = true;
        return;
    }
    std::fprintf(stderr, "awssdk: could not wake q's event loop; retrying on the next event\n");
}

}  // namespace

void post_to_main(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        queue().push_back(std::move(fn));
    }
    wake_if_needed();
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
    wake_if_needed();
}

}
