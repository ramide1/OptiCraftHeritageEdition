#include "platform/Log.h"
#include "java/JavaNetwork.h"

// Console builds that select this fallback have no socket backend. Wii builds
// with networking enabled exclude this translation unit and use JavaNetwork_wii.cpp.
// CTR_PLATFORM: same treatment -- a 3DS build with 3DS_ENABLE_NETWORK=ON excludes
// this file and uses the native soc[] backend in src/3ds/JavaNetwork_3ds.cpp.
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM) || defined(CTR_PLATFORM)

#include <istream>
#include <ostream>

namespace JavaNetwork
{
std::unique_ptr<Socket> createSocket()                                    { return nullptr; }
std::unique_ptr<std::istream> createInputStream(Socket &)                 { return nullptr; }
std::unique_ptr<std::ostream> createOutputStream(Socket &)                { return nullptr; }
bool readUrl(const std::string &, std::vector<unsigned char> &)           { return false; }
int  getResponseCode(const std::string &)                                 { return -1; }
bool postUrl(const std::string &, const std::string &, const std::string &,
             std::vector<unsigned char> &)                              { return false; }
}

#else

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>

#ifdef _WIN32
// Winsock first: windows.h (pulled by game headers) must never come before
// it, or the older winsock.h shadows these declarations.
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace JavaNetwork
{

namespace
{

// Native desktop sockets (Winsock on Windows, BSD sockets elsewhere): the
// Wii, 3DS and PS2 backends all ride their own stacks, so the desktop was
// the only SDL_net user left -- and SDL_net v2 exposed no socket options,
// which is why desktop multiplayer never got the TCP_NODELAY every console
// backend sets. Shape follows JavaNetwork_3ds.cpp: atomic-fd lifecycle,
// non-blocking sockets, select-sliced waits, single write mutex.
//
// No dependency, no init beyond Winsock's own (refcounted once, never torn
// down -- process exit reclaims it, the way SDL_Quit was never called).

#ifdef _WIN32
using NativeHandle = SOCKET;
constexpr NativeHandle kInvalidHandle = INVALID_SOCKET;
constexpr int kShutdownBoth = SD_BOTH;
#else
using NativeHandle = int;
constexpr NativeHandle kInvalidHandle = -1;
constexpr int kShutdownBoth = SHUT_RDWR;
#endif

// Handshake budget. DNS runs uncapped on the calling thread (connect and
// server-list threads only -- the old blocking resolver did the same),
// while the TCP handshake below is bounded so a filtered host fails fast
// instead of parking the thread in the kernel's SYN-retry window.
constexpr int kConnectTimeoutSeconds = 10;
constexpr int kReadSliceMs = 100;      // matches the old CheckSockets cadence
constexpr int kWriteStallBudgetMs = 10000; // 3DS parity: no-progress cap

constexpr int kSendFlags =
#if defined(__linux__)
    MSG_NOSIGNAL; // a write to a peer-closed socket reports EPIPE instead of killing the process
#else
    0;
#endif

#ifdef _WIN32
void ensureWSAInit()
{
    static std::once_flag flag;
    std::call_once(flag, []()
    {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
            MC_LOG_ERROR("network", "WSAStartup failed: %d\n", ::WSAGetLastError());
    });
}
#endif

void closeHandle(NativeHandle handle)
{
#ifdef _WIN32
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

bool setNonBlocking(NativeHandle handle)
{
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(handle, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(handle, F_GETFL, 0);
    return flags >= 0 && ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool lastErrorWouldBlock()
{
#ifdef _WIN32
    return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int rawSend(NativeHandle handle, const char *data, int length)
{
#ifdef _WIN32
    return ::send(handle, data, length, kSendFlags);
#else
    return static_cast<int>(::send(handle, data, static_cast<std::size_t>(length), kSendFlags));
#endif
}

int rawRecv(NativeHandle handle, char *data, int length)
{
#ifdef _WIN32
    return ::recv(handle, data, length, 0);
#else
    return static_cast<int>(::recv(handle, data, static_cast<std::size_t>(length), 0));
#endif
}

// select() for writability (write=true) or readability. A null timeout
// waits indefinitely, mirroring the old blocking-socket shape.
int waitSocket(NativeHandle handle, bool write, const struct timeval *timeout)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(handle, &set);
#ifdef _WIN32
    if (write)
        return ::select(0, nullptr, &set, nullptr, timeout);
    return ::select(0, &set, nullptr, nullptr, timeout);
#else
    if (write)
        return ::select(handle + 1, nullptr, &set, nullptr, timeout);
    return ::select(handle + 1, &set, nullptr, nullptr, timeout);
#endif
}

bool sendAllNative(NativeHandle handle, const char *data, int length)
{
    int offset = 0;
    while (offset < length)
    {
        const int count = rawSend(handle, data + offset, length - offset);
        if (count > 0)
        {
            offset += count;
            continue;
        }
        if (count < 0 && !lastErrorWouldBlock())
            return false;
        if (waitSocket(handle, true, nullptr) <= 0)
            return false;
    }
    return true;
}

void applySocketOptions(NativeHandle handle)
{
    // Player packets are small and latency-sensitive: disable Nagle so the
    // stack does not deliberately hold them waiting for a coalescing
    // partner, same as the Wii/3DS/PS2 backends. Non-fatal.
    const int noDelay = 1;
    (void)::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
#ifdef _WIN32
        reinterpret_cast<const char *>(&noDelay),
#else
        &noDelay,
#endif
        sizeof(noDelay));
#if defined(__APPLE__)
    const int noSigPipe = 1;
    (void)::setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
}

// Opens a client TCP connection (IPv4, like the old SDL_net path).
// Returns kInvalidHandle on failure.
NativeHandle openNativeConnection(const std::string &host, int port)
{
#ifdef _WIN32
    ensureWSAInit();
#endif
    if (host.empty() || port < 1 || port > 65535)
        return kInvalidHandle;

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<unsigned short>(port));

    // Literal IPs first: no resolver round-trip for the common raw-address case.
    if (::inet_pton(AF_INET, host.c_str(), &target.sin_addr) != 1)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *resolvedHead = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &resolvedHead) != 0 ||
            resolvedHead == nullptr)
        {
            if (resolvedHead != nullptr)
                ::freeaddrinfo(resolvedHead);
            return kInvalidHandle;
        }
        bool resolvedOk = false;
        for (addrinfo *entry = resolvedHead; entry != nullptr; entry = entry->ai_next)
        {
            if (entry->ai_family == AF_INET && entry->ai_addr != nullptr)
            {
                std::memcpy(&target.sin_addr,
                    &reinterpret_cast<sockaddr_in *>(entry->ai_addr)->sin_addr,
                    sizeof(target.sin_addr));
                resolvedOk = true;
                break;
            }
        }
        ::freeaddrinfo(resolvedHead);
        if (!resolvedOk)
            return kInvalidHandle;
    }

#ifdef _WIN32
    const NativeHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidHandle)
        return kInvalidHandle;
#else
    const int rawFd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (rawFd < 0)
        return kInvalidHandle;
    const NativeHandle handle = rawFd;
#endif

    applySocketOptions(handle);
    if (!setNonBlocking(handle))
    {
        closeHandle(handle);
        return kInvalidHandle;
    }

    if (::connect(handle, reinterpret_cast<sockaddr *>(&target), sizeof(target)) == 0)
        return handle; // localhost: already established

#ifdef _WIN32
    if (::WSAGetLastError() != WSAEWOULDBLOCK)
#else
    if (errno != EINPROGRESS)
#endif
    {
        closeHandle(handle);
        return kInvalidHandle;
    }

    // Bounded handshake in short slices (the 3DS shape): a failing connect
    // may never flag writability, so one long select would burn the whole
    // budget without answering. Completion is probed the standard way --
    // SO_ERROR through getsockopt -- which desktop stacks answer properly.
    const auto start = std::chrono::steady_clock::now();
    const auto budget = std::chrono::seconds(kConnectTimeoutSeconds);
    for (;;)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= budget)
            break;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(budget - elapsed);
        const auto slice = std::min(remaining, std::chrono::milliseconds(kReadSliceMs));
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = static_cast<long>(slice.count()) * 1000L;

        const int ready = waitSocket(handle, true, &tv);
        if (ready < 0)
        {
            closeHandle(handle);
            return kInvalidHandle;
        }
        if (ready == 0)
            continue;
        int socketError = 0;
        bool probeOk = true;
#ifdef _WIN32
        int errorLen = sizeof(socketError);
        if (::getsockopt(handle, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char *>(&socketError), &errorLen) != 0)
            probeOk = false;
#else
        socklen_t errorLen = sizeof(socketError);
        if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, &socketError, &errorLen) != 0)
            probeOk = false;
#endif
        if (!probeOk || socketError != 0)
        {
            closeHandle(handle);
            return kInvalidHandle;
        }
        return handle;
    }
    closeHandle(handle);
    return kInvalidHandle;
}

class PcNativeSocket final : public Socket
{
public:
    ~PcNativeSocket() override { releaseSocket(); }

    bool connect(const std::string &host, int port) override
    {
        releaseSocket();
        closing.store(false, std::memory_order_release);
        readInterrupted.store(false, std::memory_order_release);
        receivedBytes.store(0, std::memory_order_relaxed);
        sentBytes.store(0, std::memory_order_relaxed);
        remoteSocketAddress = host + ":" + std::to_string(port);

        MC_LOG_INFO("network", "TCP connect: %s\n", remoteSocketAddress.c_str());
        const NativeHandle handle = openNativeConnection(host, port);
        if (handle == kInvalidHandle)
        {
            MC_LOG_WARN("network", "TCP connect failed: %s\n", remoteSocketAddress.c_str());
            releaseSocket();
            return false;
        }
        fd.store(handle, std::memory_order_release);
        MC_LOG_INFO("network", "TCP connected: %s\n", remoteSocketAddress.c_str());
        return true;
    }

    int read(char *buffer, int length) override
    {
        NativeHandle socketFd = fd.load(std::memory_order_acquire);
        if (socketFd == kInvalidHandle || buffer == nullptr || length <= 0 ||
            closing.load(std::memory_order_acquire))
            return -1;

        // Non-blocking sockets (see openNativeConnection): recv-first saves
        // a select round-trip per packet when data is already queued --
        // every packet during a map stream. The wait runs in slices so
        // close()/interruptRead() from another thread take effect promptly;
        // after each wait the fd is re-validated because close() may have
        // retired (and a later connection recycled) the descriptor mid-sleep.
        while (!closing.load(std::memory_order_acquire) &&
               !readInterrupted.load(std::memory_order_acquire))
        {
            const int count = rawRecv(socketFd, buffer, length);
            if (count > 0)
            {
                receivedBytes.fetch_add(static_cast<std::size_t>(count),
                    std::memory_order_relaxed);
                return count;
            }
            if (count == 0)
                return -1; // peer closed
            if (!lastErrorWouldBlock())
                return -1; // real read error

            struct timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = static_cast<long>(kReadSliceMs) * 1000L;
            const int ready = waitSocket(socketFd, false, &tv);
            // Re-check after the wait: close() may have retired this
            // descriptor while select() slept, and its number can already
            // have been recycled by a later connection.
            if (fd.load(std::memory_order_acquire) != socketFd ||
                closing.load(std::memory_order_acquire) ||
                readInterrupted.load(std::memory_order_acquire))
                return -1;
            if (ready < 0)
                return -1;
            // ready == 0 (slice elapsed) or readable: loop, and the
            // non-blocking recv re-samples for real.
        }
        return -1;
    }

    bool write(const char *buffer, int length) override
    {
        if (buffer == nullptr)
            return false;
        if (length <= 0)
            return true;

        // Serialized like the 3DS backend: the write thread and any
        // disconnect flush can both push bytes through this socket.
        std::lock_guard<std::mutex> guard(writeLock);
        int offset = 0;
        while (offset < length)
        {
            const NativeHandle socketFd = fd.load(std::memory_order_acquire);
            if (socketFd == kInvalidHandle || closing.load(std::memory_order_acquire))
                return false;

            const int count = rawSend(socketFd, buffer + offset, length - offset);
            if (count > 0)
            {
                sentBytes.fetch_add(static_cast<std::size_t>(count),
                    std::memory_order_relaxed);
                offset += count;
                continue;
            }
            if (count < 0 && !lastErrorWouldBlock())
                return false;
            // Kernel buffer full: wait for writability in slices -- close()
            // takes effect within one slice, and a peer that stops draining
            // for good surfaces as a failed write (a disconnect) instead of
            // a wedged thread. The budget resets on progress only, so a
            // healthy but slow peer is never cut off.
            int waitedMs = 0;
            while (!closing.load(std::memory_order_acquire))
            {
                struct timeval tv{};
                tv.tv_sec = 0;
                tv.tv_usec = static_cast<long>(kReadSliceMs) * 1000L;
                const int ready = waitSocket(socketFd, true, &tv);
                if (fd.load(std::memory_order_acquire) != socketFd ||
                    closing.load(std::memory_order_acquire))
                    return false;
                if (ready < 0)
                    return false;
                if (ready > 0)
                    break; // writable again -- retry the send

                waitedMs += kReadSliceMs;
                if (waitedMs >= kWriteStallBudgetMs)
                    return false;
            }
        }
        return true;
    }

    bool flush() override
    {
        return fd.load(std::memory_order_acquire) != kInvalidHandle &&
               !closing.load(std::memory_order_acquire);
    }

    void interruptRead() override
    {
        // Flag only: the select-sliced reader notices within one slice,
        // and this avoids shutting down a descriptor another thread may be
        // retiring concurrently.
        readInterrupted.store(true, std::memory_order_release);
    }

    void close() override
    {
        closing.store(true, std::memory_order_release);
        readInterrupted.store(true, std::memory_order_release);
        const NativeHandle socketFd = fd.exchange(kInvalidHandle, std::memory_order_acq_rel);
        if (socketFd != kInvalidHandle)
        {
            ::shutdown(socketFd, kShutdownBoth);
            closeHandle(socketFd);
        }
    }

    std::string getRemoteSocketAddress() const override { return remoteSocketAddress; }
    std::size_t getReceivedByteCount() const override
    {
        return receivedBytes.load(std::memory_order_relaxed);
    }
    std::size_t getSentByteCount() const override
    {
        return sentBytes.load(std::memory_order_relaxed);
    }

private:
    void releaseSocket()
    {
        closing.store(true, std::memory_order_release);
        readInterrupted.store(true, std::memory_order_release);
        const NativeHandle socketFd = fd.exchange(kInvalidHandle, std::memory_order_acq_rel);
        if (socketFd != kInvalidHandle)
        {
            ::shutdown(socketFd, kShutdownBoth);
            closeHandle(socketFd);
        }
    }

    std::atomic<NativeHandle> fd{kInvalidHandle};
    std::atomic_bool closing{true};
    std::atomic_bool readInterrupted{false};
    std::atomic<std::size_t> receivedBytes{0};
    std::atomic<std::size_t> sentBytes{0};
    std::mutex writeLock;
    std::string remoteSocketAddress;
};

class SocketInputBuffer : public std::streambuf
{
public:
    explicit SocketInputBuffer(Socket &socket)
        : socket(socket)
    {
        setg(buffer, buffer, buffer);
    }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());

        int count = socket.read(buffer, sizeof(buffer));
        if (count <= 0)
            return traits_type::eof();

        setg(buffer, buffer, buffer + count);
        return traits_type::to_int_type(*gptr());
    }

private:
    Socket &socket;
    // 8 KiB, not the 512 bytes this used to be: every underflow() is one
    // select() + recv() syscall round-trip, and the istream-driven Packet
    // reader asks in streambuf-sized gulps. A login burst or a map-chunk
    // packet (tens of KiB) crossed 512 bytes in hundreds of round-trips --
    // each with its own kernel context switch. Same change the 3DS backend
    // carries for its IPC round-trips; the growth lives on the heap-held
    // istream, not on any thread stack.
    char buffer[8192];
};

class SocketOutputBuffer : public std::streambuf
{
public:
    explicit SocketOutputBuffer(Socket &socket)
        : socket(socket)
    {
        setp(buffer, buffer + sizeof(buffer));
    }

    ~SocketOutputBuffer() override
    {
        sync();
    }

protected:
    std::streamsize xsputn(const char *s, std::streamsize n) override
    {
        std::streamsize written = 0;
        while (written < n)
        {
            std::streamsize space = epptr() - pptr();
            if (space == 0)
            {
                if (!flushBuffer())
                    return written;
                space = epptr() - pptr();
            }

            const std::streamsize remaining = n - written;
            const std::streamsize count = remaining < space ? remaining : space;
            std::memcpy(pptr(), s + written, static_cast<std::size_t>(count));
            pbump(static_cast<int>(count));
            written += count;
        }
        return written;
    }

    int_type overflow(int_type ch) override
    {
        if (traits_type::eq_int_type(ch, traits_type::eof()))
            return traits_type::not_eof(ch);
        if (!flushBuffer())
            return traits_type::eof();

        *pptr() = traits_type::to_char_type(ch);
        pbump(1);
        return ch;
    }

    int sync() override
    {
        return flushBuffer() && socket.flush() ? 0 : -1;
    }

private:
    bool flushBuffer()
    {
        const std::streamsize count = pptr() - pbase();
        if (count > 0 && !socket.write(pbase(), static_cast<int>(count)))
            return false;
        pbump(-static_cast<int>(count));
        return true;
    }

    Socket &socket;
    char buffer[5120];
};

class SocketInputStream : public std::istream
{
public:
    explicit SocketInputStream(Socket &socket)
        : std::istream(nullptr)
        , buffer(socket)
    {
        rdbuf(&buffer);
    }

private:
    SocketInputBuffer buffer;
};

class SocketOutputStream : public std::ostream
{
public:
    explicit SocketOutputStream(Socket &socket)
        : std::ostream(nullptr)
        , buffer(socket)
    {
        rdbuf(&buffer);
    }

private:
    SocketOutputBuffer buffer;
};

}

std::unique_ptr<Socket> createSocket()
{
    return std::make_unique<PcNativeSocket>();
}

std::unique_ptr<std::istream> createInputStream(Socket &socket)
{
    return std::make_unique<SocketInputStream>(socket);
}

std::unique_ptr<std::ostream> createOutputStream(Socket &socket)
{
    return std::make_unique<SocketOutputStream>(socket);
}

namespace
{

// Minimal HTTP/1.0 GET over the native socket. Returns the HTTP status code
// (or -1 on a connection/parse failure) and fills body with the response
// payload. Only plain http:// is supported -- there is no TLS here, so
// https is rejected.
int httpGet(const std::string &url, std::vector<unsigned char> &body)
{
    const std::string scheme = "http://";
    if (url.rfind(scheme, 0) != 0)
        return -1;

    std::string rest = url.substr(scheme.size());
    std::string::size_type pathStart = rest.find('/');
    std::string hostPort = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);
    std::string path = pathStart == std::string::npos ? "/" : rest.substr(pathStart);

    std::string host = hostPort;
    int port = 80;
    std::string::size_type colon = hostPort.find(':');
    if (colon != std::string::npos)
    {
        host = hostPort.substr(0, colon);
        port = std::atoi(hostPort.c_str() + colon + 1);
    }

    NativeHandle socket = openNativeConnection(host, port);
    if (socket == kInvalidHandle)
        return -1;

    std::string request =
        "GET " + path + " HTTP/1.0\r\n" +
        "Host: " + host + "\r\n" +
        "User-Agent: Minecraft\r\n" +
        "Connection: close\r\n\r\n";
    if (sendAllNative(socket, request.data(), static_cast<int>(request.size())))
    {
        std::string raw;
        char chunk[2048];
        for (;;)
        {
            if (waitSocket(socket, false, nullptr) < 0)
                break;
            const int count = rawRecv(socket, chunk, sizeof(chunk));
            if (count <= 0)
                break;
            raw.append(chunk, count);
        }

        if (!raw.empty())
        {
            int status = -1;
            std::string::size_type sp = raw.find(' ');
            if (sp != std::string::npos)
                status = std::atoi(raw.c_str() + sp + 1);

            std::string::size_type headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos)
            {
                const char *bodyStart = raw.data() + headerEnd + 4;
                std::size_t bodyLen = raw.size() - (headerEnd + 4);
                body.assign(bodyStart, bodyStart + bodyLen);
            }
            closeHandle(socket);
            return status;
        }
    }
    closeHandle(socket);
    return -1;
};

int httpPost(const std::string &url, const std::string &contentType,
             const std::string &body, std::vector<unsigned char> &response)
{
    const std::string scheme = "http://";
    if (url.rfind(scheme, 0) != 0)
        return -1;

    std::string rest = url.substr(scheme.size());
    std::string::size_type pathStart = rest.find('/');
    std::string hostPort = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);
    std::string path = pathStart == std::string::npos ? "/" : rest.substr(pathStart);
    std::string host = hostPort;
    int port = 80;
    const std::string::size_type colon = hostPort.find(':');
    if (colon != std::string::npos)
    {
        host = hostPort.substr(0, colon);
        port = std::atoi(hostPort.c_str() + colon + 1);
    }

    NativeHandle socket = openNativeConnection(host, port);
    if (socket == kInvalidHandle)
        return -1;

    const std::string request =
        "POST " + path + " HTTP/1.0\r\n" +
        "Host: " + host + "\r\n" +
        "User-Agent: Minecraft\r\n" +
        "Content-Type: " + contentType + "\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Content-Language: en-US\r\n" +
        "Connection: close\r\n\r\n" + body;

    if (sendAllNative(socket, request.data(), static_cast<int>(request.size())))
    {
        std::string raw;
        char chunk[2048];
        for (;;)
        {
            if (waitSocket(socket, false, nullptr) < 0)
                break;
            const int count = rawRecv(socket, chunk, sizeof(chunk));
            if (count <= 0)
                break;
            raw.append(chunk, count);
        }

        if (!raw.empty())
        {
            int status = -1;
            const std::string::size_type sp = raw.find(' ');
            if (sp != std::string::npos)
                status = std::atoi(raw.c_str() + sp + 1);

            const std::string::size_type headerEnd = raw.find("\r\n\r\n");
            response.clear();
            if (headerEnd != std::string::npos)
            {
                const char *begin = raw.data() + headerEnd + 4;
                response.assign(begin, begin + (raw.size() - headerEnd - 4));
            }
            closeHandle(socket);
            return status;
        }
    }
    closeHandle(socket);
    return -1;
}

}

bool readUrl(const std::string &url, std::vector<unsigned char> &data)
{
    if (url.rfind("http://", 0) == 0)
    {
        int status = httpGet(url, data);
        return status >= 200 && status < 300;
    }

    std::string file = url;
    if (file.rfind("file://", 0) == 0)
        file = file.substr(7);

    if (file.find("://") != std::string::npos)
        return false;

    std::ifstream input(file, std::ios::binary);
    if (!input)
        return false;

    input.seekg(0, std::ios::end);
    std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size < 0)
        return false;

    data.resize((std::size_t)size);
    if (!data.empty())
        input.read(reinterpret_cast<char *>(data.data()), size);
    return true;
}

int getResponseCode(const std::string &url)
{
    if (url.rfind("http://", 0) == 0)
    {
        std::vector<unsigned char> body;
        return httpGet(url, body);
    }

    std::vector<unsigned char> data;
    return readUrl(url, data) ? 200 : -1;
};

bool postUrl(const std::string &url, const std::string &contentType,
             const std::string &body, std::vector<unsigned char> &response)
{
    if (url.rfind("http://", 0) != 0)
        return false;
    const int status = httpPost(url, contentType, body, response);
    return status >= 200 && status < 300;
}

}

#endif // !PS2_PLATFORM;s