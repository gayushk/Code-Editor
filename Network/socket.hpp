#pragma once

#include <vector>
#include <string_view>
#include <sys/types.h>

namespace SimpleNet {
	
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

	std::vector<char> receive(size_t max_size = 4096);
	ssize_t send(std::string_view msg);

private:
	explicit Socket(int fd) : fd_(fd) {}
	int fd_ = -1;	
};	

};
