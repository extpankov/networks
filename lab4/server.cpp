#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "common.hpp"

void osi_recv()
{
    std::cout << "[Layer 4 - Transport]     recv()\n";
    std::cout << "[Layer 6 - Presentation]  deserialize Message\n";
}

void osi_auth_ok(const std::string& nick)
{
    std::cout << "[Layer 5 - Session]       authenticated: "
              << nick << "\n";
}

void osi_auth_fail(const std::string& reason)
{
    std::cout << "[Layer 5 - Session]       auth failed: "
              << reason << "\n";
}

void osi_handle(const std::string& type)
{
    std::cout << "[Layer 7 - Application]   handle " << type << "\n";
}

void osi_send()
{
    std::cout << "[Layer 7 - Application]   prepare response\n";
    std::cout << "[Layer 6 - Presentation]  serialize Message\n";
    std::cout << "[Layer 4 - Transport]     send()\n";
}

struct Client {
    int         fd;
    std::string nick;
    std::string peer;
};

std::vector<Client> g_clients;
std::mutex          g_mtx;

bool nick_taken(const std::string& nick)
{
    for (const auto& c : g_clients)
        if (c.nick == nick)
            return true;
    return false;
}

void clients_add(int fd, const std::string& nick, const std::string& peer)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_clients.push_back({fd, nick, peer});
}

void clients_remove(int fd)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
        if (it->fd == fd) {
            g_clients.erase(it);
            break;
        }
    }
}

void broadcast(const Message& m, int exclude = -1)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& c : g_clients) {
        if (c.fd == exclude) continue;
        try { io::send(c.fd, m); } catch (...) {}
    }
}

bool send_private(const std::string& to_nick, const Message& m)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& c : g_clients) {
        if (c.nick == to_nick) {
            try { io::send(c.fd, m); } catch (...) {}
            return true;
        }
    }
    return false;
}

class ThreadPool
{
public:
    ThreadPool(size_t n)
    {
        for (size_t i = 0; i < n; i++)
            workers_.push_back(std::thread([this]{ loop(); }));
    }

    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    void submit(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.push(task);
        }
        cv_.notify_one();
    }

private:
    void loop()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this]{
                    return stop_ || !tasks_.empty();
                });
                if (stop_ && tasks_.empty()) return;
                task = tasks_.front();
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mtx_;
    std::condition_variable           cv_;
    bool                              stop_ = false;
};


std::string peer_str(const sockaddr_in& a)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

bool parse_private(const std::string& payload,
                   std::string& to_nick,
                   std::string& text)
{
    auto pos = payload.find(':');
    if (pos == std::string::npos || pos == 0)
        return false;
    to_nick = payload.substr(0, pos);
    text    = payload.substr(pos + 1);
    return true;
}

void handle(int fd, std::string peer)
{
    std::string nick;

    auto cleanup = [&]()
    {
        clients_remove(fd);
        close(fd);
        if (!nick.empty()) {
            broadcast(Message::make(MsgType::SERVER_INFO,
                "User [" + nick + "] disconnected"));
            std::cout << "User [" << nick << "] disconnected\n\n";
        }
    };

    try
    {
        osi_recv();
        Message msg = io::recv(fd);

        if (msg.type != MsgType::HELLO)
            throw NetError("expected HELLO");

        osi_send();
        io::send(fd, Message::make(MsgType::WELCOME, "connected"));

        osi_recv();
        msg = io::recv(fd);
        std::cout << "[Layer 6 - Presentation]  parsed MSG_AUTH\n";

        if (msg.type != MsgType::AUTH) {
            osi_auth_fail("expected AUTH");
            osi_send();
            io::send(fd, Message::make(MsgType::ERROR, "send AUTH first"));
            close(fd);
            return;
        }

        nick = msg.text();

        if (nick.empty()) {
            osi_auth_fail("empty nick");
            osi_send();
            io::send(fd, Message::make(MsgType::ERROR, "nick cannot be empty"));
            close(fd);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_mtx);
            if (nick_taken(nick)) {
                osi_auth_fail("nick taken: " + nick);
                io::send(fd, Message::make(MsgType::ERROR,
                    "nick [" + nick + "] already taken"));
                close(fd);
                return;
            }
        }

        osi_auth_ok(nick);

        osi_send();
        io::send(fd, Message::make(MsgType::SERVER_INFO,
            "Auth OK, welcome " + nick + "!"));

        clients_add(fd, nick, peer);

        broadcast(Message::make(MsgType::SERVER_INFO,
            "User [" + nick + "] connected"), fd);
        std::cout << "User [" << nick << "] connected\n\n";

        for (;;)
        {
            osi_recv();
            msg = io::recv(fd);

            if (msg.type == MsgType::TEXT)
            {
                osi_handle("MSG_TEXT");
                std::string text = "[" + nick + "]: " + msg.text();
                std::cout << "[Layer 7 - Application]   broadcast: "
                          << text << "\n\n";
                broadcast(Message::make(MsgType::TEXT, text));
            }
            else if (msg.type == MsgType::PRIVATE)
            {
                osi_handle("MSG_PRIVATE");

                std::string to_nick, text;
                if (!parse_private(msg.text(), to_nick, text)) {
                    osi_send();
                    io::send(fd, Message::make(MsgType::ERROR,
                        "bad format, use: nick:message"));
                    continue;
                }

                std::string out = "[PRIVATE][" + nick + "]: " + text;
                std::cout << "[Layer 7 - Application]   private "
                          << nick << " -> " << to_nick << "\n\n";

                if (!send_private(to_nick,
                        Message::make(MsgType::PRIVATE, out))) {
                    osi_send();
                    io::send(fd, Message::make(MsgType::ERROR,
                        "user [" + to_nick + "] not found"));
                }
            }
            else if (msg.type == MsgType::PING)
            {
                osi_handle("MSG_PING");
                osi_send();
                io::send(fd, Message::make(MsgType::PONG));
            }
            else if (msg.type == MsgType::BYE)
            {
                osi_handle("MSG_BYE");
                break;
            }
        }
    }
    catch (const NetError& e) {
        std::cerr << "[error] " << peer << ": " << e.what() << "\n";
    }

    cleanup();
}

int main()
{
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::cerr << "socket error\n"; return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind error\n"; return 1;
    }
    if (listen(srv, 32) < 0) {
        std::cerr << "listen error\n"; return 1;
    }

    std::cout << "[server] port " << PORT
              << ", pool " << POOL_SIZE << "\n\n";

    ThreadPool pool(POOL_SIZE);

    for (;;)
    {
        sockaddr_in ca;
        memset(&ca, 0, sizeof(ca));
        socklen_t cl = sizeof(ca);

        int fd = accept(srv, (sockaddr*)&ca, &cl);
        if (fd < 0) continue;

        std::string peer = peer_str(ca);
        std::cout << "Client connected: " << peer << "\n";

        pool.submit([fd, peer](){ handle(fd, peer); });
    }

    return 0;
}
