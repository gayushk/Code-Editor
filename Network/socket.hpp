#pragma once

#include <vector>
#include <string_view>
#include <sys/types.h>
#include <fcntl.h>
#include <stdexcept>

namespace SimpleNet {

enum class RecvStatus {Ok, WouldBlock, Closed};

struct RecvResult {
	std::vector<char> data;
	RecvStatus status;
};

class Socket {
	
public: 
	int get_fd() const {return fd_;}
	Socket();
	~Socket();

	Socket(const Socket&) = delete;
	Socket& operator=(const Socket&) = delete;

	Socket(Socket&& oth) noexcept;
	Socket& operator=(Socket&& oth) noexcept;

	void bind(int port);
	void listen(int backlog = 10);
	void connect(const std::string& ip, int port);

	Socket accept();
	RecvResult receive(size_t max_size = 4096);
	ssize_t send(std::string_view msg);

private:
	explicit Socket(int fd) : fd_(fd) {}
	int fd_ = -1;
};

inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) throw std::runtime_error("fcntl F_GETFL");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        throw std::runtime_error("fcntl F_SETFL");
}

};

