#include "socket.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>
#include <utility>
#include <arpa/inet.h>
#include <string>

namespace SimpleNet {

Socket::Socket() {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ == -1) {
        throw std::runtime_error("Failed to create socket");
    }
}

Socket::~Socket() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

Socket::Socket(Socket&& other) noexcept : fd_{other.fd_} {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (fd_ != -1) ::close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;

}

void Socket::bind(int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        throw std::runtime_error("Failed to bind to port " + std::to_string(port));
    }
}

void Socket::listen(int backlog) {
    if(::listen(fd_, backlog) == -1) {
     throw std::runtime_error("Failed to listen on socket");
    }
}
void Socket::connect(const std::string& ip, int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        throw std::runtime_error("Invalid IP address");
    }

    if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("Failed to connect to server");
    }
}

Socket Socket::accept() {
    int client_fd = ::accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
     throw std::runtime_error("Failed to accept connection");
    }
    return Socket(client_fd);
}

std::vector<char> Socket::receive(size_t max_size) {
    std::vector<char> buffer(max_size);
    
    ssize_t recieved  = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (recieved == -1) {
     throw std::runtime_error("Failed to recieve data");
    }
    
    buffer.resize(recieved);
    return buffer;
}


ssize_t Socket::send(std::string_view data) {
    ssize_t sent = ::send(fd_, data.data(), data.size(), 0);
    if (sent == -1) {
        throw std::runtime_error("Failed to send data");
    }
    return sent;
}

}; 
