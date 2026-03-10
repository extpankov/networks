#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include "message.h"

#define PORT 8080
#define BUFFER_SIZE 1024

bool receive_message(int sock, Message* msg) {
    uint32_t net_length = 0;

    if (recv(sock, &net_length, sizeof(net_length), MSG_WAITALL) <= 0) {
        return false;
    }

    msg->length = ntohl(net_length);

    if (recv(sock, &msg->type, sizeof(msg->type), MSG_WAITALL) <= 0) {
        return false;
    }

    int payload_length = msg->length - sizeof(msg->type);

    if (payload_length > 0) {
        if (recv(sock, msg->payload, payload_length, MSG_WAITALL) <= 0) {
            return false;
        }
        msg->payload[payload_length] = '\0';
    } else {
        msg->payload[0] = '\0';
    }

    return true;
}

bool send_message(int sock, uint8_t type, const char* payload) {
    uint32_t payload_length = strlen(payload);
    uint32_t full_length = sizeof(type) + payload_length;
    uint32_t net_length = htonl(full_length);

    if (send(sock, &net_length, sizeof(net_length), 0) < 0) {
        return false;
    }

    if (send(sock, &type, sizeof(type), 0) < 0) {
        return false;
    }

    if (payload_length > 0) {
        if (send(sock, payload, payload_length, 0) < 0) {
            return false;
        }
    }

    return true;
}

void receive_thread(int sock, bool* running) {
    Message msg;

    while (*running) {
        if (!receive_message(sock, &msg)) {
            std::cout << "Disconnected" << std::endl;
            *running = false;
            break;
        }

        if (msg.type == MSG_TEXT) {
            std::cout << msg.payload << std::endl;
        } 
        else if (msg.type == MSG_PONG) {
            std::cout << "PONG" << std::endl;
        } 
        else if (msg.type == MSG_BYE) {
            std::cout << "Disconnected" << std::endl;
            *running = false;
            break;
        } 
        else {
            std::cout << "Unknown message type: " << (int)msg.type << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cout << "Socket creation error" << std::endl;
        return 1;
    }

    char nickname[256];

    if (argc > 1) {
        strncpy(nickname, argv[1], sizeof(nickname) - 1);
        nickname[sizeof(nickname) - 1] = '\0';
    } else {
        std::cout << "Enter your nickname: ";
        std::cin.getline(nickname, sizeof(nickname));
    }

    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        std::cout << "Invalid address" << std::endl;
        close(sock);
        return 1;
    }

    if (connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cout << "Connection failed" << std::endl;
        close(sock);
        return 1;
    }

    std::cout << "Connected" << std::endl;

    if (!send_message(sock, MSG_HELLO, nickname)) {
        std::cout << "Failed to send HELLO message" << std::endl;
        close(sock);
        return 1;
    }

    Message msg;
    if (!receive_message(sock, &msg)) {
        std::cout << "Failed to receive WELCOME message" << std::endl;
        close(sock);
        return 1;
    }

    if (msg.type != MSG_WELCOME) {
        std::cout << "Expected MSG_WELCOME, got type: " << (int)msg.type << std::endl;
        close(sock);
        return 1;
    }

    std::cout << msg.payload << std::endl;

    bool running = true;
    std::thread t(receive_thread, sock, &running);

    char input[BUFFER_SIZE];

    while (running) {
        std::cout << "> ";
        std::cout.flush();

        if (!std::cin.getline(input, sizeof(input))) {
            break;
        }

        if (strcmp(input, "/ping") == 0) {
            if (!send_message(sock, MSG_PING, "")) {
                std::cout << "Failed to send PING message" << std::endl;
                break;
            }
        }
        else if (strcmp(input, "/quit") == 0) {
            send_message(sock, MSG_BYE, "");
            running = false;
            break;
        }
        else {
            if (!send_message(sock, MSG_TEXT, input)) {
                std::cout << "Failed to send TEXT message" << std::endl;
                break;
            }
        }
    }

    running = false;

    if (t.joinable()) {
        t.join();
    }

    close(sock);
    return 0;
}