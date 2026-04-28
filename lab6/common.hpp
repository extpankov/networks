#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <stdexcept>
#include <sys/socket.h>

constexpr int PORT            = 8080;
constexpr int POOL_SIZE       = 4;
constexpr int RECONNECT_DELAY = 3;
constexpr int DEFAULT_HISTORY = 20;
constexpr int MAX_NAME        = 32;
constexpr int MAX_PAYLOAD     = 256;

enum MsgType : uint8_t {
    HELLO        = 1,
    WELCOME      = 2,
    TEXT         = 3,
    PING         = 4,
    PONG         = 5,
    BYE          = 6,
    AUTH         = 7,
    PRIVATE      = 8,
    MSG_ERROR    = 9,
    SERVER_INFO  = 10,
    LIST         = 11,
    HISTORY      = 12,
    HISTORY_DATA = 13,
    HELP         = 14,
    ACK          = 15
};

inline const char* type_str(uint8_t t) {
    switch (t) {
        case HELLO:        return "MSG_HELLO";
        case WELCOME:      return "MSG_WELCOME";
        case TEXT:         return "MSG_TEXT";
        case PING:         return "MSG_PING";
        case PONG:         return "MSG_PONG";
        case BYE:          return "MSG_BYE";
        case AUTH:         return "MSG_AUTH";
        case PRIVATE:      return "MSG_PRIVATE";
        case MSG_ERROR:    return "MSG_ERROR";
        case SERVER_INFO:  return "MSG_SERVER_INFO";
        case LIST:         return "MSG_LIST";
        case HISTORY:      return "MSG_HISTORY";
        case HISTORY_DATA: return "MSG_HISTORY_DATA";
        case HELP:         return "MSG_HELP";
        case ACK:          return "MSG_ACK";
        default:           return "MSG_UNKNOWN";
    }
}

struct MessageEx {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char     sender[MAX_NAME];
    char     receiver[MAX_NAME];
    time_t   timestamp;
    char     payload[MAX_PAYLOAD];

    static MessageEx make(uint8_t t, const std::string& text = "",
                          const std::string& from = "",
                          const std::string& to = "",
                          uint32_t id = 0) {
        MessageEx m{};
        m.type      = t;
        m.msg_id    = id;
        m.timestamp = std::time(nullptr);
        std::strncpy(m.sender,   from.c_str(), MAX_NAME - 1);
        std::strncpy(m.receiver, to.c_str(),   MAX_NAME - 1);
        size_t len = std::min(text.size(), (size_t)MAX_PAYLOAD - 1);
        std::memcpy(m.payload, text.c_str(), len);
        m.payload[len] = '\0';
        m.length = (uint32_t)len;
        return m;
    }

    std::string text() const { return std::string(payload, length); }
};

inline std::string fmt_time(time_t ts) {
    char buf[32];
    std::tm* tm = std::localtime(&ts);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    return buf;
}

struct NetError : std::runtime_error {
    explicit NetError(const std::string& s) : std::runtime_error(s) {}
};

namespace io {

inline void send_all(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n > 0) {
        ssize_t r = ::send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) throw NetError("send failed");
        p += r; n -= r;
    }
}

inline void recv_all(int fd, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n > 0) {
        ssize_t r = ::recv(fd, p, n, 0);
        if (r <= 0) throw NetError("recv failed");
        p += r; n -= r;
    }
}

inline void send(int fd, const MessageEx& m) {
    send_all(fd, &m, sizeof(MessageEx));
}

inline MessageEx recv(int fd) {
    MessageEx m{};
    recv_all(fd, &m, sizeof(MessageEx));
    return m;
}

}
