#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <csignal>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "common.hpp"

static std::atomic<uint32_t> g_msg_id{1};

uint32_t next_id() {
    return g_msg_id.fetch_add(1);
}

struct OfflineMsg {
    char     sender[MAX_NAME];
    char     receiver[MAX_NAME];
    char     text[MAX_PAYLOAD];
    time_t   timestamp;
    uint32_t msg_id;
};

struct HistoryRecord {
    uint32_t    msg_id;
    time_t      timestamp;
    std::string sender;
    std::string receiver;
    std::string type;
    std::string text;
    bool        delivered;
    bool        is_offline;
};

struct Client {
    int         fd;
    std::string nick;
    std::string peer;
};

static std::vector<Client> g_clients;
static std::mutex g_clients_mtx;

static std::unordered_map<std::string, std::vector<OfflineMsg>> g_offline;
static std::mutex g_offline_mtx;

static std::mutex g_hist_mtx;
static std::mutex g_log_mtx;

const char* HISTORY_FILE = "chat_history.json";

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else                out += c;
    }
    return out;
}

static std::string to_json(const HistoryRecord& r) {
    std::ostringstream os;
    os << "{"
       << "\"msg_id\":"     << r.msg_id << ","
       << "\"timestamp\":"  << (long long)r.timestamp << ","
       << "\"sender\":\""   << json_escape(r.sender)   << "\","
       << "\"receiver\":\"" << json_escape(r.receiver) << "\","
       << "\"type\":\""     << r.type << "\","
       << "\"text\":\""     << json_escape(r.text) << "\","
       << "\"delivered\":"  << (r.delivered  ? "true" : "false") << ","
       << "\"is_offline\":" << (r.is_offline ? "true" : "false")
       << "}";
    return os.str();
}

static void history_append(const HistoryRecord& r) {
    std::lock_guard<std::mutex> lock(g_hist_mtx);
    std::ofstream f(HISTORY_FILE, std::ios::app);
    if (f) f << to_json(r) << "\n";
}

static std::vector<HistoryRecord> history_read(int n) {
    std::lock_guard<std::mutex> lock(g_hist_mtx);

    std::ifstream f(HISTORY_FILE);
    if (!f) return {};

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) lines.push_back(line);

    if (n > 0 && (int)lines.size() > n)
        lines.erase(lines.begin(), lines.end() - n);

    auto get_str = [](const std::string& json, const std::string& key) {
        std::string pat = "\"" + key + "\":\"";
        auto pos = json.find(pat);
        if (pos == std::string::npos) return std::string("");
        pos += pat.size();
        std::string val;
        bool esc = false;
        for (; pos < json.size(); ++pos) {
            char c = json[pos];
            if (esc) { val += c; esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') break;
            val += c;
        }
        return val;
    };

    auto get_num = [](const std::string& json, const std::string& key) {
        std::string pat = "\"" + key + "\":";
        auto pos = json.find(pat);
        if (pos == std::string::npos) return 0LL;
        pos += pat.size();
        long long v = 0;
        while (pos < json.size() && std::isdigit((unsigned char)json[pos]))
            v = v * 10 + (json[pos++] - '0');
        return v;
    };

    auto get_bool = [](const std::string& json, const std::string& key) {
        std::string pat = "\"" + key + "\":";
        auto pos = json.find(pat);
        if (pos == std::string::npos) return false;
        return json.compare(pos + pat.size(), 4, "true") == 0;
    };

    std::vector<HistoryRecord> recs;
    for (const auto& ln : lines) {
        HistoryRecord r;
        r.msg_id     = (uint32_t)get_num(ln, "msg_id");
        r.timestamp  = (time_t)  get_num(ln, "timestamp");
        r.sender     = get_str(ln, "sender");
        r.receiver   = get_str(ln, "receiver");
        r.type       = get_str(ln, "type");
        r.text       = get_str(ln, "text");
        r.delivered  = get_bool(ln, "delivered");
        r.is_offline = get_bool(ln, "is_offline");
        recs.push_back(r);
    }
    return recs;
}

static std::string format_record(const HistoryRecord& r) {
    std::ostringstream os;
    os << "[" << fmt_time(r.timestamp) << "][id=" << r.msg_id << "]";
    if (r.is_offline)
        os << "[OFFLINE][" << r.sender << " -> " << r.receiver << "]: " << r.text;
    else if (r.type == "MSG_PRIVATE")
        os << "[" << r.sender << " -> " << r.receiver << "][PRIVATE]: " << r.text;
    else
        os << "[" << r.sender << "]: " << r.text;
    return os.str();
}

static bool nick_taken(const std::string& nick) {
    for (const auto& c : g_clients)
        if (c.nick == nick) return true;
    return false;
}

static void clients_add(int fd, const std::string& nick, const std::string& peer) {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    g_clients.push_back({fd, nick, peer});
}

static void clients_remove(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
        if (it->fd == fd) { g_clients.erase(it); break; }
    }
}

static std::string clients_list() {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    std::string res = "Online users:\n";
    for (const auto& c : g_clients) res += c.nick + "\n";
    return res;
}

static void broadcast(const MessageEx& m, int exclude = -1) {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    for (auto& c : g_clients) {
        if (c.fd == exclude) continue;
        try { io::send(c.fd, m); } catch (...) {}
    }
}

static bool send_to(const std::string& nick, const MessageEx& m) {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    for (auto& c : g_clients) {
        if (c.nick == nick) {
            try { io::send(c.fd, m); return true; } catch (...) {}
        }
    }
    return false;
}

static void offline_store(const OfflineMsg& om) {
    std::lock_guard<std::mutex> lock(g_offline_mtx);
    g_offline[om.receiver].push_back(om);
}

static std::vector<OfflineMsg> offline_pop(const std::string& nick) {
    std::lock_guard<std::mutex> lock(g_offline_mtx);
    auto it = g_offline.find(nick);
    if (it == g_offline.end()) return {};
    auto msgs = std::move(it->second);
    g_offline.erase(it);
    return msgs;
}

static void log_recv(const MessageEx& m, const std::string& peer, size_t bytes) {
    std::lock_guard<std::mutex> lock(g_log_mtx);
    std::string src_ip = peer;
    std::string src_port = "?";
    auto colon = peer.rfind(':');
    if (colon != std::string::npos) {
        src_ip   = peer.substr(0, colon);
        src_port = peer.substr(colon + 1);
    }
    std::cout << "[Network Access] frame arrived from NIC\n"
              << "[Internet]       src=" << src_ip << " dst=127.0.0.1 proto=TCP\n"
              << "[Transport]      recv() " << bytes << " bytes"
              << "  src_port=" << src_port << " dst_port=" << PORT << "\n"
              << "[Application]    deserialize MessageEx -> "
              << type_str(m.type) << " from " << m.sender << "\n\n";
}

static void log_send(const MessageEx& m, const std::string& peer, size_t bytes) {
    std::lock_guard<std::mutex> lock(g_log_mtx);
    std::string dst_ip = peer;
    auto colon = peer.rfind(':');
    if (colon != std::string::npos) dst_ip = peer.substr(0, colon);
    std::cout << "[Application]    prepare " << type_str(m.type) << "\n"
              << "[Transport]      send() " << bytes << " bytes via TCP\n"
              << "[Internet]       dst=" << dst_ip << " proto=TCP\n"
              << "[Network Access] frame sent to network interface\n\n";
}

static void srv_send(int fd, MessageEx m, const std::string& peer) {
    log_send(m, peer, sizeof(MessageEx));
    io::send(fd, m);
}

class ThreadPool {
public:
    ThreadPool(size_t n) {
        for (size_t i = 0; i < n; i++)
            workers.push_back(std::thread([this]{ loop(); }));
    }

    ~ThreadPool() {
        { std::unique_lock<std::mutex> lk(mtx); stop = true; }
        cv.notify_all();
        for (auto& t : workers) t.join();
    }

    void submit(std::function<void()> task) {
        { std::lock_guard<std::mutex> lk(mtx); tasks.push(task); }
        cv.notify_one();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [this]{ return stop || !tasks.empty(); });
                if (stop && tasks.empty()) return;
                task = tasks.front();
                tasks.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop = false;
};

static std::string peer_str(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

static void handle(int fd, std::string peer) {
    std::string nick;

    auto cleanup = [&]() {
        clients_remove(fd);
        close(fd);
        if (!nick.empty()) {
            broadcast(MessageEx::make(SERVER_INFO,
                "User [" + nick + "] disconnected", "SERVER", "", next_id()));
            std::lock_guard<std::mutex> lk(g_log_mtx);
            std::cout << "User [" << nick << "] disconnected\n\n";
        }
    };

    try {
        MessageEx msg = io::recv(fd);
        log_recv(msg, peer, sizeof(MessageEx));

        if (msg.type != HELLO) throw NetError("expected HELLO");

        srv_send(fd, MessageEx::make(WELCOME, "connected", "SERVER", msg.sender, next_id()), peer);

        msg = io::recv(fd);
        log_recv(msg, peer, sizeof(MessageEx));

        if (msg.type != AUTH) {
            srv_send(fd, MessageEx::make(MSG_ERROR, "send MSG_AUTH first", "SERVER", "", next_id()), peer);
            close(fd);
            return;
        }

        nick = msg.text();

        if (nick.empty()) {
            srv_send(fd, MessageEx::make(MSG_ERROR, "nick cannot be empty", "SERVER", "", next_id()), peer);
            close(fd);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_clients_mtx);
            if (nick_taken(nick)) {
                io::send(fd, MessageEx::make(MSG_ERROR,
                    "nick [" + nick + "] already taken", "SERVER", nick, next_id()));
                close(fd);
                return;
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_log_mtx);
            std::cout << "[Application]    authentication success: " << nick << "\n"
                      << "[Application]    SYN -> ACK -> READY\n"
                      << "[Application]    coffee powered TCP/IP stack initialized\n"
                      << "[Application]    packets never sleep\n\n";
        }

        srv_send(fd, MessageEx::make(SERVER_INFO,
            "Auth OK, welcome " + nick + "!", "SERVER", nick, next_id()), peer);

        clients_add(fd, nick, peer);

        broadcast(MessageEx::make(SERVER_INFO,
            "User [" + nick + "] connected", "SERVER", "", next_id()), fd);

        {
            std::lock_guard<std::mutex> lk(g_log_mtx);
            std::cout << "User [" << nick << "] connected\n\n";
        }

        auto offline = offline_pop(nick);

        if (offline.empty()) {
            std::lock_guard<std::mutex> lk(g_log_mtx);
            std::cout << "[Application]    no offline messages for " << nick << "\n\n";
        } else {
            std::lock_guard<std::mutex> lk(g_log_mtx);
            std::cout << "[Application]    delivering " << offline.size()
                      << " offline message(s) to " << nick << "\n\n";
        }

        for (const auto& om : offline) {
            std::string display =
                "[" + fmt_time(om.timestamp) + "]"
                "[id=" + std::to_string(om.msg_id) + "]"
                "[OFFLINE][" + om.sender + " -> " + nick + "]: " + om.text;

            auto m = MessageEx::make(PRIVATE, display, om.sender, nick, om.msg_id);
            m.timestamp = om.timestamp;
            srv_send(fd, m, peer);

            history_append({om.msg_id, om.timestamp, om.sender, nick,
                            "MSG_PRIVATE", om.text, true, true});
        }

        if (!offline.empty()) {
            srv_send(fd, MessageEx::make(SERVER_INFO,
                "message delivered (maybe)", "SERVER", nick, next_id()), peer);
        }

        for (;;) {
            msg = io::recv(fd);
            log_recv(msg, peer, sizeof(MessageEx));

            if (msg.type == TEXT) {
                std::string display =
                    "[" + fmt_time(msg.timestamp) + "]"
                    "[id=" + std::to_string(msg.msg_id) + "]"
                    "[" + nick + "]: " + msg.text();

                {
                    std::lock_guard<std::mutex> lk(g_log_mtx);
                    std::cout << "[Application]    broadcast: " << display << "\n\n";
                }

                auto bcast = MessageEx::make(TEXT, display, nick, "", msg.msg_id);
                bcast.timestamp = msg.timestamp;
                broadcast(bcast);

                history_append({msg.msg_id, msg.timestamp, nick, "",
                                "MSG_TEXT", msg.text(), true, false});
            }
            else if (msg.type == PRIVATE) {
                std::string to_nick = msg.receiver;
                std::string text    = msg.text();

                if (to_nick.empty()) {
                    srv_send(fd, MessageEx::make(MSG_ERROR,
                        "set receiver field for MSG_PRIVATE", "SERVER", nick, next_id()), peer);
                    continue;
                }

                std::string display =
                    "[" + fmt_time(msg.timestamp) + "]"
                    "[id=" + std::to_string(msg.msg_id) + "]"
                    "[PRIVATE][" + nick + " -> " + to_nick + "]: " + text;

                auto pm = MessageEx::make(PRIVATE, display, nick, to_nick, msg.msg_id);
                pm.timestamp = msg.timestamp;

                if (send_to(to_nick, pm)) {
                    {
                        std::lock_guard<std::mutex> lk(g_log_mtx);
                        std::cout << "[Application]    private "
                                  << nick << " -> " << to_nick << "\n\n";
                    }
                    history_append({msg.msg_id, msg.timestamp, nick, to_nick,
                                    "MSG_PRIVATE", text, true, false});
                } else {
                    {
                        std::lock_guard<std::mutex> lk(g_log_mtx);
                        std::cout << "[Application]    receiver " << to_nick << " is offline\n"
                                  << "[Application]    store message in offline queue\n"
                                  << "[Application]    if it works - don't touch it\n"
                                  << "[Application]    append record to history file delivered=false\n\n";
                    }

                    OfflineMsg om{};
                    std::strncpy(om.sender,   nick.c_str(),    MAX_NAME - 1);
                    std::strncpy(om.receiver, to_nick.c_str(), MAX_NAME - 1);
                    std::strncpy(om.text,     text.c_str(),    MAX_PAYLOAD - 1);
                    om.timestamp = msg.timestamp;
                    om.msg_id    = msg.msg_id;
                    offline_store(om);

                    history_append({msg.msg_id, msg.timestamp, nick, to_nick,
                                    "MSG_PRIVATE", text, false, true});

                    srv_send(fd, MessageEx::make(SERVER_INFO,
                        "User [" + to_nick + "] is offline. Message stored.",
                        "SERVER", nick, next_id()), peer);
                }
            }
            else if (msg.type == LIST) {
                srv_send(fd, MessageEx::make(SERVER_INFO,
                    clients_list(), "SERVER", nick, next_id()), peer);
            }
            else if (msg.type == HISTORY) {
                int n = DEFAULT_HISTORY;
                std::string param = msg.text();
                if (!param.empty()) {
                    try { n = std::stoi(param); }
                    catch (...) {
                        srv_send(fd, MessageEx::make(MSG_ERROR,
                            "invalid argument for /history", "SERVER", nick, next_id()), peer);
                        continue;
                    }
                    if (n <= 0) {
                        srv_send(fd, MessageEx::make(MSG_ERROR,
                            "N must be positive", "SERVER", nick, next_id()), peer);
                        continue;
                    }
                }

                auto recs = history_read(n);
                std::ostringstream os;
                for (const auto& r : recs)
                    os << format_record(r) << "\n";
                os << "[LOG]: i love TCP/IP (don't tell UDP)";

                srv_send(fd, MessageEx::make(HISTORY_DATA,
                    os.str(), "SERVER", nick, next_id()), peer);
            }
            else if (msg.type == PING) {
                srv_send(fd, MessageEx::make(PONG, "", "SERVER", nick, next_id()), peer);
            }
            else if (msg.type == BYE) {
                break;
            }
        }
    }
    catch (const NetError& e) {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        std::cerr << "[error] " << peer << ": " << e.what() << "\n";
    }

    cleanup();
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::cerr << "socket error\n"; return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind error\n"; return 1;
    }
    if (listen(srv, 32) < 0) {
        std::cerr << "listen error\n"; return 1;
    }

    std::cout << "[server] port " << PORT << "  pool=" << POOL_SIZE << "\n\n";

    ThreadPool pool(POOL_SIZE);

    for (;;) {
        sockaddr_in ca{};
        socklen_t cl = sizeof(ca);
        int fd = accept(srv, (sockaddr*)&ca, &cl);
        if (fd < 0) continue;

        std::string peer = peer_str(ca);
        std::cout << "Client connected: " << peer << "\n";

        pool.submit([fd, peer]{ handle(fd, peer); });
    }

    return 0;
}
