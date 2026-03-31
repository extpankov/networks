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

struct Client {
    int fd;
    std::string nick;
    std::string peer;
};

std::vector<Client> g_clients;
std::mutex g_mtx;

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
    for (int i = 0; i < (int)g_clients.size(); i++) {
        if (g_clients[i].fd == exclude)
            continue;
        try {
            io::send(g_clients[i].fd, m);
        }
        catch (...) {
        }
    }
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
        for (int i = 0; i < (int)workers_.size(); i++)
            workers_[i].join();
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
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this]{
                    return stop_ || !tasks_.empty();
                });
                if (stop_ && tasks_.empty())
                    return;
                task = tasks_.front();
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
};

std::string peer_str(const sockaddr_in& a)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    std::string result = std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
    return result;
}

void handle(int fd, std::string peer)
{
    std::string nick;

    auto cleanup = [&]()
    {
        clients_remove(fd);
        close(fd);
        if (!nick.empty()) {
            broadcast(Message::make(MsgType::TEXT,
                "*** " + nick + " left the chat ***"));
            std::cout << "[disconnected] " << nick
                      << " (" << peer << ")\n";
        }
    };

    try
    {
        Message msg = io::recv(fd);
        if (msg.type != MsgType::HELLO)
            throw NetError("no HELLO");

        nick = msg.text();
        std::cout << "[hello] " << nick << " (" << peer << ")\n";

        io::send(fd, Message::make(MsgType::WELCOME, "Welcome!"));

        clients_add(fd, nick, peer);
        broadcast(Message::make(MsgType::TEXT,
            "*** " + nick + " joined ***"), fd);

        for (;;)
        {
            msg = io::recv(fd);

            if (msg.type == MsgType::TEXT) {
                std::string text = nick + " [" + peer + "]: " + msg.text();
                std::cout << "[broadcast] " << text << "\n";
                broadcast(Message::make(MsgType::TEXT, text));
            }
            else if (msg.type == MsgType::PING) {
                io::send(fd, Message::make(MsgType::PONG));
            }
            else if (msg.type == MsgType::BYE) {
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
    if (srv < 0) {
        std::cerr << "socket error\n";
        return 1;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind error\n";
        return 1;
    }

    if (listen(srv, 32) < 0) {
        std::cerr << "listen error\n";
        return 1;
    }

    std::cout << "[server] port " << PORT
              << ", pool " << POOL_SIZE << "\n";

    ThreadPool pool(POOL_SIZE);

    for (;;)
    {
        sockaddr_in ca;
        memset(&ca, 0, sizeof(ca));
        socklen_t cl = sizeof(ca);

        int fd = accept(srv, (sockaddr*)&ca, &cl);
        if (fd < 0)
            continue;

        std::string peer = peer_str(ca);
        std::cout << "[accept] " << peer << "\n";

        pool.submit([fd, peer](){ handle(fd, peer); });
    }

    return 0;
}
