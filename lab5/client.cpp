#include <iostream>
#include <string>
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

static int               g_fd        = -1;
static std::atomic<bool> g_connected { false };
static std::atomic<bool> g_running   { true  };
static std::mutex        g_mtx;
static std::string       g_nick;
static std::string       g_host = "127.0.0.1";

static std::atomic<uint32_t> g_msg_id{1};

uint32_t next_id() {
    return g_msg_id.fetch_add(1);
}

static void disconnect() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_connected = false;
}

static bool send_msg(MessageEx m) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_fd < 0) return false;
    try { io::send(g_fd, m); return true; }
    catch (...) { g_connected = false; return false; }
}

static void recv_loop() {
    while (g_running) {
        if (!g_connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        int fd;
        { std::lock_guard<std::mutex> lk(g_mtx); fd = g_fd; }

        try {
            MessageEx msg = io::recv(fd);

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
            else if (msg.type == PONG) {
                std::cout << "\r[SERVER]: PONG\n"
                          << "[LOG]: i love cast (no segfaults pls)\n"
                          << "> " << std::flush;
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

    std::lock_guard<std::mutex> lk(g_mtx);
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

static void print_help() {
    std::cout << "Available commands:\n"
                 "/help\n"
                 "/list\n"
                 "/history\n"
                 "/history N\n"
                 "/quit\n"
                 "/w <nick> <message>\n"
                 "/ping\n"
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
            send_msg(MessageEx::make(BYE, "", g_nick, "", next_id()));
            break;
        }
        else if (line == "/help") {
            print_help();
            continue;
        }
        else if (line == "/ping") {
            send_msg(MessageEx::make(PING, "", g_nick, "", next_id()));
        }
        else if (line == "/list") {
            send_msg(MessageEx::make(LIST, "", g_nick, "", next_id()));
        }
        else if (line == "/history") {
            send_msg(MessageEx::make(HISTORY, "", g_nick, "", next_id()));
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
            send_msg(MessageEx::make(HISTORY, param, g_nick, "", next_id()));
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
            send_msg(pm);
        }
        else if (!line.empty()) {
            send_msg(MessageEx::make(TEXT, line, g_nick, "", next_id()));
        }

        if (g_running)
            std::cout << "> " << std::flush;
    }

    g_running = false;
    disconnect();
    std::cout << "[*] bye\n";
    return 0;
}
