#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <csignal>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "common.hpp"

using clock_t2 = std::chrono::steady_clock;
using ms_double = std::chrono::duration<double, std::milli>;

static int               g_fd        = -1;
static std::atomic<bool> g_connected { false };
static std::atomic<bool> g_running   { true  };
static std::mutex        g_fd_mtx;
static std::string       g_nick;
static std::string       g_host = "127.0.0.1";

static std::atomic<uint32_t> g_msg_id{1};
uint32_t next_id() { return g_msg_id.fetch_add(1); }

struct PendingMsg {
    MessageEx msg;
    clock_t2::time_point send_time;
    int retries;
};

static std::unordered_map<uint32_t, PendingMsg> g_pending;
static std::mutex g_pending_mtx;

struct PingEntry {
    clock_t2::time_point send_time;
    bool answered;
    double rtt_ms;
};

static std::unordered_map<uint32_t, PingEntry> g_pings;
static std::mutex g_pings_mtx;

struct NetStats {
    std::vector<double> rtts;
    std::vector<double> jitters;
    int sent    = 0;
    int lost    = 0;
};

static NetStats g_stats;
static std::mutex g_stats_mtx;

static void disconnect() {
    std::lock_guard<std::mutex> lk(g_fd_mtx);
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_connected = false;
}

static bool raw_send(MessageEx m) {
    std::lock_guard<std::mutex> lk(g_fd_mtx);
    if (g_fd < 0) return false;
    try { io::send(g_fd, m); return true; }
    catch (...) { g_connected = false; return false; }
}

static bool send_reliable(MessageEx m) {
    constexpr int MAX_RETRIES = 3;
    constexpr int TIMEOUT_MS  = 2000;

    uint32_t id = m.msg_id;

    {
        std::lock_guard<std::mutex> lk(g_pending_mtx);
        g_pending[id] = { m, clock_t2::now(), 0 };
    }

    std::cout << "[Transport][RETRY] send " << type_str(m.type)
              << " (id=" << id << ")\n";

    if (!raw_send(m)) return false;

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        auto deadline = clock_t2::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);

        while (clock_t2::now() < deadline) {
            {
                std::lock_guard<std::mutex> lk(g_pending_mtx);
                if (!g_pending.count(id)) {
                    std::cout << "[Transport][RETRY] ACK received (id="
                              << id << ")\n";
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (attempt < MAX_RETRIES) {
            std::cout << "[Transport][RETRY] wait ACK timeout\n"
                      << "[Transport][RETRY] resend " << attempt + 1
                      << "/" << MAX_RETRIES << " (id=" << id << ")\n";
            raw_send(m);
        }
    }

    std::cout << "[Transport][RETRY] failed to deliver (id=" << id << ")\n";
    std::lock_guard<std::mutex> lk(g_pending_mtx);
    g_pending.erase(id);
    return false;
}

static void recv_loop() {
    while (g_running) {
        if (!g_connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        int fd;
        { std::lock_guard<std::mutex> lk(g_fd_mtx); fd = g_fd; }

        try {
            MessageEx msg = io::recv(fd);

            if (msg.type == ACK) {
                std::lock_guard<std::mutex> lk(g_pending_mtx);
                g_pending.erase(msg.msg_id);
                continue;
            }

            if (msg.type == PONG) {
                auto now = clock_t2::now();
                double rtt = -1.0;
                double jitter = -1.0;

                {
                    std::lock_guard<std::mutex> lk(g_pings_mtx);
                    auto it = g_pings.find(msg.msg_id);
                    if (it != g_pings.end()) {
                        rtt = ms_double(now - it->second.send_time).count();
                        it->second.answered = true;
                        it->second.rtt_ms   = rtt;
                    }
                }

                {
                    std::lock_guard<std::mutex> lk(g_stats_mtx);
                    if (rtt >= 0) {
                        if (!g_stats.rtts.empty()) {
                            double prev = g_stats.rtts.back();
                            jitter = std::abs(rtt - prev);
                            g_stats.jitters.push_back(jitter);
                        }
                        g_stats.rtts.push_back(rtt);
                    }
                }

                std::ostringstream line;
                line << "\rRTT=" << rtt << "ms";
                if (jitter >= 0) line << " | Jitter=" << jitter << "ms";
                line << "\n> ";
                std::cout << line.str() << std::flush;
                continue;
            }

            if (msg.type == TEXT || msg.type == PRIVATE) {
                std::cout << "\r" << msg.text() << "\n> " << std::flush;
            }
            else if (msg.type == HISTORY_DATA) {
                std::cout << "\r--- history ---\n"
                          << msg.text()
                          << "\n---------------\n> " << std::flush;
            }
            else if (msg.type == SERVER_INFO) {
                std::cout << "\r[SERVER]: " << msg.text() << "\n> " << std::flush;
            }
            else if (msg.type == MSG_ERROR) {
                std::cout << "\r[ERROR]: " << msg.text() << "\n> " << std::flush;
            }
            else if (msg.type == BYE) {
                std::cout << "\r[kicked]\n" << std::flush;
                g_running = false;
            }
        }
        catch (...) {
            if (!g_running) break;
            std::cout << "\n[!] disconnected\n> " << std::flush;
            disconnect();
        }
    }
}

static bool do_connect() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    inet_pton(AF_INET, g_host.c_str(), &srv.sin_addr);

    if (connect(fd, (sockaddr*)&srv, sizeof(srv)) < 0) {
        close(fd); return false;
    }

    try {
        auto hello = MessageEx::make(HELLO, g_nick, g_nick, "", next_id());
        io::send(fd, hello);

        MessageEx msg = io::recv(fd);
        if (msg.type != WELCOME) throw NetError("no WELCOME");

        auto auth = MessageEx::make(AUTH, g_nick, g_nick, "", next_id());
        io::send(fd, auth);

        msg = io::recv(fd);
        if (msg.type == MSG_ERROR) {
            std::cout << "[AUTH ERROR]: " << msg.text() << "\n";
            close(fd); return false;
        }
        if (msg.type != SERVER_INFO) throw NetError("unexpected AUTH response");
        std::cout << "[server]: " << msg.text() << "\n";
    }
    catch (const NetError& e) {
        std::cerr << "[!] " << e.what() << "\n";
        close(fd); return false;
    }

    std::lock_guard<std::mutex> lk(g_fd_mtx);
    if (g_fd >= 0) close(g_fd);
    g_fd = fd;
    g_connected = true;
    return true;
}

static void reconnect_loop() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (g_connected) continue;
        std::cout << "\r[*] reconnecting...\n> " << std::flush;
        if (do_connect())
            std::cout << "\r[*] reconnected as " << g_nick << "\n> " << std::flush;
        else
            std::cout << "\r[!] failed, retry in 2s\n> " << std::flush;
    }
}

static void do_ping_series(int count) {
    constexpr int TIMEOUT_MS = 2000;

    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        g_stats = {};
        g_stats.sent = count;
    }

    for (int i = 1; i <= count; i++) {
        uint32_t id = next_id();
        auto now = clock_t2::now();

        {
            std::lock_guard<std::mutex> lk(g_pings_mtx);
            g_pings[id] = { now, false, -1.0 };
        }

        std::cout << "PING " << i << " → ";
        std::cout.flush();

        auto ping_msg = MessageEx::make(PING, "", g_nick, "", id);
        if (!raw_send(ping_msg)) {
            std::cout << "send error\n";
            continue;
        }

        auto deadline = clock_t2::now() + std::chrono::milliseconds(TIMEOUT_MS);
        bool got = false;

        while (clock_t2::now() < deadline) {
            {
                std::lock_guard<std::mutex> lk(g_pings_mtx);
                auto it = g_pings.find(id);
                if (it != g_pings.end() && it->second.answered) {
                    got = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (!got) {
            std::cout << "timeout\n";
            std::lock_guard<std::mutex> lk(g_stats_mtx);
            g_stats.lost++;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

static void do_netdiag() {
    double rtt_avg = 0.0, jitter_avg = 0.0, loss = 0.0;

    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);

        if (!g_stats.rtts.empty()) {
            for (double v : g_stats.rtts) rtt_avg += v;
            rtt_avg /= (double)g_stats.rtts.size();
        }

        if (!g_stats.jitters.empty()) {
            for (double v : g_stats.jitters) jitter_avg += v;
            jitter_avg /= (double)g_stats.jitters.size();
        }

        if (g_stats.sent > 0)
            loss = (double)g_stats.lost / (double)g_stats.sent * 100.0;
    }

    std::ostringstream os;
    os.precision(1);
    os << std::fixed;
    os << "RTT avg : " << rtt_avg    << " ms\n"
       << "Jitter  : " << jitter_avg << " ms\n"
       << "Loss    : " << loss       << " %";

    std::cout << "\n" << os.str() << "\n\n";

    std::string fname = "net_diag_" + g_nick + ".json";
    std::ofstream f(fname);
    if (f) {
        f << "{\n"
          << "  \"nick\": \""   << g_nick    << "\",\n"
          << "  \"rtt_avg\": "  << rtt_avg   << ",\n"
          << "  \"jitter\": "   << jitter_avg << ",\n"
          << "  \"loss_pct\": " << loss       << ",\n"
          << "  \"sent\": "     << g_stats.sent << ",\n"
          << "  \"lost\": "     << g_stats.lost << "\n"
          << "}\n";
        std::cout << "[saved to " << fname << "]\n";
    }
}

static void print_help() {
    std::cout << "Available commands:\n"
                 "/help\n"
                 "/list\n"
                 "/history\n"
                 "/history N\n"
                 "/ping\n"
                 "/ping N\n"
                 "/netdiag\n"
                 "/quit\n"
                 "/w <nick> <message>\n"
                 "Tip: meow, message delivered\n"
              << "> " << std::flush;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2) g_host = argv[1];

    std::cout << "Enter nickname: ";
    std::getline(std::cin, g_nick);
    if (g_nick.empty()) g_nick = "anon";

    while (!do_connect()) {
        std::cout << "[!] retry in " << RECONNECT_DELAY << "s\n";
        std::this_thread::sleep_for(std::chrono::seconds(RECONNECT_DELAY));
    }
    std::cout << "[*] connected as " << g_nick << "\n> " << std::flush;

    std::thread(recv_loop).detach();
    std::thread(reconnect_loop).detach();

    std::string line;
    while (g_running) {
        if (!std::getline(std::cin, line)) break;

        if (!g_connected && line != "/quit") {
            std::cout << "[!] not connected\n> " << std::flush;
            continue;
        }

        if (line == "/quit") {
            raw_send(MessageEx::make(BYE, "", g_nick, "", next_id()));
            break;
        }
        else if (line == "/help") {
            print_help();
            continue;
        }
        else if (line == "/netdiag") {
            do_netdiag();
        }
        else if (line == "/ping") {
            do_ping_series(10);
        }
        else if (line.size() > 6 && line.substr(0, 6) == "/ping ") {
            std::string param = line.substr(6);
            bool valid = !param.empty();
            for (char c : param)
                if (!std::isdigit((unsigned char)c)) { valid = false; break; }
            if (!valid || std::stoi(param) <= 0) {
                std::cout << "usage: /ping [N]\n> " << std::flush;
                continue;
            }
            do_ping_series(std::stoi(param));
        }
        else if (line == "/list") {
            raw_send(MessageEx::make(LIST, "", g_nick, "", next_id()));
        }
        else if (line == "/history") {
            raw_send(MessageEx::make(HISTORY, "", g_nick, "", next_id()));
        }
        else if (line.size() > 9 && line.substr(0, 9) == "/history ") {
            std::string param = line.substr(9);
            bool valid = !param.empty();
            for (char c : param)
                if (!std::isdigit((unsigned char)c)) { valid = false; break; }
            if (!valid) {
                std::cout << "usage: /history [N]\n> " << std::flush;
                continue;
            }
            raw_send(MessageEx::make(HISTORY, param, g_nick, "", next_id()));
        }
        else if (line.size() >= 3 && line.substr(0, 3) == "/w ") {
            std::string rest = line.substr(3);
            auto pos = rest.find(' ');
            if (pos == std::string::npos || pos == 0) {
                std::cout << "usage: /w <nick> <message>\n> " << std::flush;
                continue;
            }
            std::string to_nick = rest.substr(0, pos);
            std::string text    = rest.substr(pos + 1);
            if (text.empty()) {
                std::cout << "usage: /w <nick> <message>\n> " << std::flush;
                continue;
            }
            auto pm = MessageEx::make(PRIVATE, text, g_nick, to_nick, next_id());
            std::thread([pm]() mutable { send_reliable(pm); }).detach();
        }
        else if (!line.empty()) {
            auto tm = MessageEx::make(TEXT, line, g_nick, "", next_id());
            std::thread([tm]() mutable { send_reliable(tm); }).detach();
        }

        if (g_running)
            std::cout << "> " << std::flush;
    }

    g_running = false;
    disconnect();
    std::cout << "[*] bye\n";
    return 0;
}
