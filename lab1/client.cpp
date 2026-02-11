#include <iostream>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    char message[1024];

    while (true) {
        std::cout << "Enter message: ";
        std::cin.getline(message, 1024);

        sendto(sockfd, message, strlen(message), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));

        char buffer[1024];
        socklen_t addrLen = sizeof(serverAddr);

        int n = recvfrom(sockfd, buffer, 1024, 0, (struct sockaddr*)&serverAddr, &addrLen);

        buffer[n] = '\0';
        std::cout << "Server: " << buffer << std::endl;
    }

    close(sockfd);
    return 0;
}