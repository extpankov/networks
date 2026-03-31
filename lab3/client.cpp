#include <iostream>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "common.hpp"

int g_fd = -1;
std::atomic<bool> g_connected{false};
std::atomic<bool> g_running{true};
std::mutex g_mtx;
std::string g_nick;
std::string g_host = "127.0.0.1";

void disconnect()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    g_connected = false;
}

bool do_connect()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    inet_pton(AF_INET, g_host.c_str(), &srv.sin_addr);

    if (connect(fd, (sockaddr*)&srv, sizeof(srv)) < 0) {
        close(fd);
        return false;
    }

    try {
        io::send(fd, Message::make(MsgType::HELLO, g_nick));
        Message msg = io::recv(fd);
        if (msg.type != MsgType::WELCOME)
            throw NetError("no WELCOME");
        std::cout << "\r[server]: " << msg.text() << "\n> " << std::flush;
    }
    catch (...) {
        close(fd);
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_fd >= 0)
        close(g_fd);
    g_fd = fd;
    g_connected = true;
    return true;
}

void recv_loop()
{
    while (g_running)
    {
        if (!g_connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        int fd;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            fd = g_fd;
        }

        try {
            Message msg = io::recv(fd);

            if (msg.type == MsgType::TEXT) {
                std::cout << "\r" << msg.text() << "\n> " << std::flush;
            }
            else if (msg.type == MsgType::BYE) {
                std::cout << "\r[kicked]\n" << std::flush;
                g_running = false;
            }
        }
        catch (...) {
            if (!g_running)
                break;
            std::cout << "\n[!] disconnected\n> " << std::flush;
            disconnect();
        }
    }
}

void reconnect_loop()
{
    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        if (g_connected)
            continue;

        std::cout << "\r[*] reconnecting...\n> " << std::flush;

        if (do_connect())
            std::cout << "\r[*] reconnected as " << g_nick << "\n> " << std::flush;
        else
            std::cout << "\r[!] failed, will retry in 2s\n> " << std::flush;
    }
}

bool send_msg(const Message& m)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_fd < 0)
        return false;
    try {
        io::send(g_fd, m);
        return true;
    }
    catch (...) {
        g_connected = false;
        return false;
    }
}

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2)
        g_host = argv[1];

    std::cout << "nick: ";
    std::getline(std::cin, g_nick);
    if (g_nick.empty())
        g_nick = "anon";

    while (!do_connect()) {
        std::cout << "[!] retry in " << RECONNECT_DELAY << "s\n";
        std::this_thread::sleep_for(std::chrono::seconds(RECONNECT_DELAY));
    }
    std::cout << "[*] connected as " << g_nick << "\n> " << std::flush;

    std::thread(recv_loop).detach();
    std::thread(reconnect_loop).detach();

    std::string line;
    while (g_running)
    {
        if (!std::getline(std::cin, line))
            break;

        if (!g_connected) {
            std::cout << "[!] not connected\n> " << std::flush;
            continue;
        }

        if (line == "/quit") {
            send_msg(Message::make(MsgType::BYE));
            break;
        }
        else if (line == "/ping") {
            send_msg(Message::make(MsgType::PING));
        }
        else if (!line.empty()) {
            send_msg(Message::make(MsgType::TEXT, line));
        }

        if (g_running)
            std::cout << "> " << std::flush;
    }

    g_running = false;
    disconnect();
    std::cout << "[*] bye\n";
    return 0;
}
