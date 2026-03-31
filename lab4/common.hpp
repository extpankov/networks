#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <unistd.h>
#include <arpa/inet.h>

constexpr int    PORT            = 9090;
constexpr int    POOL_SIZE       = 10;
constexpr int    MAX_CLIENTS     = 100;
constexpr int    RECONNECT_DELAY = 2;
constexpr size_t MAX_PAYLOAD     = 1024;

enum class MsgType : uint8_t {
    HELLO       = 1,
    WELCOME     = 2,
    TEXT        = 3,
    PING        = 4,
    PONG        = 5,
    BYE         = 6,
    AUTH        = 7,
    PRIVATE     = 8,
    ERROR       = 9,
    SERVER_INFO = 10
};

struct Message {
    uint32_t length = 0;
    MsgType  type   = MsgType::HELLO;
    char     payload[MAX_PAYLOAD] = {};

    static Message make(MsgType t, std::string_view text = {})
    {
        Message m;
        m.type = t;
        size_t len = std::min(text.size(), MAX_PAYLOAD - 1);
        std::memcpy(m.payload, text.data(), len);
        m.payload[len] = '\0';
        m.length = static_cast<uint32_t>(sizeof(m.type) + len);
        return m;
    }

    std::string text() const { return {payload}; }
};

struct NetError : std::runtime_error {
    explicit NetError(const std::string& w) : std::runtime_error(w) {}
};

namespace io {

inline void write_all(int fd, const void* buf, size_t n)
{
    const char* p = static_cast<const char*>(buf);
    for (size_t done = 0; done < n; ) {
        ssize_t r = write(fd, p + done, n - done);
        if (r <= 0) throw NetError("write failed");
        done += r;
    }
}

inline void read_all(int fd, void* buf, size_t n)
{
    char* p = static_cast<char*>(buf);
    for (size_t done = 0; done < n; ) {
        ssize_t r = read(fd, p + done, n - done);
        if (r <= 0) throw NetError("read failed");
        done += r;
    }
}

inline void send(int fd, const Message& m)
{
    uint32_t nl = htonl(m.length);
    write_all(fd, &nl,     sizeof(nl));
    write_all(fd, &m.type, m.length);
}

inline Message recv(int fd)
{
    uint32_t nl = 0;
    read_all(fd, &nl, sizeof(nl));

    Message m;
    m.length = ntohl(nl);
    if (m.length == 0 || m.length > sizeof(m.type) + MAX_PAYLOAD)
        throw NetError("bad length");

    read_all(fd, &m.type, m.length);
    m.payload[m.length - sizeof(m.type)] = '\0';
    return m;
}

} // namespace io
