#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "message.h"

#define PORT 8080

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

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == 0) {
        std::cout << "Socket creation failed" << std::endl;
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cout << "setsockopt failed" << std::endl;
        close(server_fd);
        return 1;
    }

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0) {
        std::cout << "Bind failed" << std::endl;
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 1) < 0) {
        std::cout << "Listen failed" << std::endl;
        close(server_fd);
        return 1;
    }

    std::cout << "Server listening on port " << PORT << std::endl;

    socklen_t addrlen = sizeof(address);
    int client_fd = accept(server_fd, (sockaddr*)&address, &addrlen);
    if (client_fd < 0) {
        std::cout << "Accept failed" << std::endl;
        close(server_fd);
        return 1;
    }

    char* client_ip = inet_ntoa(address.sin_addr);
    int client_port = ntohs(address.sin_port);

    std::cout << "Client connected" << std::endl;

    Message msg;

    if (!receive_message(client_fd, &msg)) {
        std::cout << "Failed to receive HELLO message" << std::endl;
        close(client_fd);
        close(server_fd);
        return 1;
    }

    if (msg.type != MSG_HELLO) {
        std::cout << "Expected MSG_HELLO, got type: " << (int)msg.type << std::endl;
        close(client_fd);
        close(server_fd);
        return 1;
    }

    std::cout << "[" << client_ip << ":" << client_port << "]: " << msg.payload << std::endl;

    char welcome_msg[256];
    snprintf(welcome_msg, sizeof(welcome_msg), "Welcome %s:%d", client_ip, client_port);

    if (!send_message(client_fd, MSG_WELCOME, welcome_msg)) {
        std::cout << "Failed to send WELCOME message" << std::endl;
        close(client_fd);
        close(server_fd);
        return 1;
    }

    while (true) {
        if (!receive_message(client_fd, &msg)) {
            std::cout << "Client disconnected" << std::endl;
            break;
        }

        if (msg.type == MSG_TEXT) {
            std::cout << "[" << client_ip << ":" << client_port << "]: " << msg.payload << std::endl;
        } 
        else if (msg.type == MSG_PING) {
            if (!send_message(client_fd, MSG_PONG, "")) {
                std::cout << "Failed to send PONG message" << std::endl;
                break;
            }
        } 
        else if (msg.type == MSG_BYE) {
            std::cout << "Client disconnected" << std::endl;
            break;
        } 
        else {
            std::cout << "Unknown message type: " << (int)msg.type << std::endl;
        }
    }

    close(client_fd);
    close(server_fd);

    return 0;
}