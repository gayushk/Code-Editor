#include "socket.hpp"
#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <string>
#include <mutex>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <functional>
#include <condition_variable>
#include <sys/epoll.h>


constexpr int PORT = 8484;
constexpr size_t THREAD_POOL_SIZE = 8;

class ThreadPool {
public:
	explicit ThreadPool(size_t n = THREAD_POOL_SIZE) {
		stop = false;
		for(size_t i = 0; i < n; ++i) {
			workers.emplace_back([this] {
				while(true) {
					std::function<void()> task;
					{
						std::unique_lock<std::mutex> lock_{q_mu};
						cv.wait(lock_, [this] {return stop || 
								       !tasks.empty();});
						if(stop && tasks.empty()) return;
						task = std::move(tasks.front());
						tasks.pop();
					}
					task();
				}
			});		
		}
	}

	~ThreadPool() {
	   {	
		std::unique_lock<std::mutex> lock_{q_mu};
		stop = true;
	   }
	   cv.notify_all();
   	   for(auto& t : workers) {
	   	if(t.joinable()) t.join();
	   }	   
	}

	void enqueue(std::function<void()> task) {
		{
			std::unique_lock<std::mutex> lock_{q_mu};
			if(stop) return;
			tasks.push(std::move(task));
		}
		cv.notify_one();
	}

private:
	std::vector<std::thread> workers;
	std::queue<std::function<void()>> tasks;
	std::mutex q_mu;
	std::condition_variable cv;
	bool stop = false;	
};


struct File {
	std::string content;
	std::mutex mu;
};


int main() {
	try{
		File fl;
		ThreadPool pool;

		SimpleNet::Socket listener;
		listener.bind(PORT);
		listener.listen(32);

		int flags = fcntl(listener.get_fd(), F_GETFL, 0);
		fcntl(listener.get_fd(), F_SETFL, flags | O_NONBLOCK);

		int epfd = epoll_create1(0);
		if (epfd < 0) throw std::runtime_error("epoll_create");

		struct epoll_event ev{};
		ev.events = EPOLLIN;
		ev.data.fd = listener.get_fd();
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, listener.get_fd(), &ev) < 0) {
			throw std::runtime_error("epoll_ctl");
		}
		std::unordered_map<int, SimpleNet::Socket> clients;

		std::cout << "terminal editor server\n" << "port: " << PORT 
			  << "\n" << "worker threads: " << THREAD_POOL_SIZE << "\n";

		struct epoll_event events[128];
		while (true) {
			int nfds = epoll_wait(epfd, events, 128, -1);
			if(nfds < 0) {
				if (errno == EINTR) continue;
				throw std::runtime_error("epoll_wait");
			}
			for(int i = 0; i < nfds; ++i) {
				int fd = events[i].data.fd;
				if (fd == listener.get_fd()){
					try {
						auto client = listener.accept();
						int cfd = client.get_fd();
						flags = fcntl(cfd, F_GETFL, 0);
						fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
						ev.events = EPOLLIN | EPOLLRDHUP;
						ev.data.fd = cfd;
						if(epoll_ctl(epfd, 
							     EPOLL_CTL_ADD, 
							     cfd, &ev) < 0) {
							close(fd);
							continue;
						}
						clients[cfd] = std::move(client);
						{
						     std::lock_guard<std::mutex> lock_{fl.mu};
						     std::string msg = fl.content + "\n";
						     clients[cfd].send(msg);
						}
						std::cout << "'+' client" << cfd << "\n";
					} catch (const std::exception& e) {
						std::cerr << "catch error: " 
							  << e.what() << "\n";
					}
					continue;
				}
				if(events[i].events & (EPOLLIN | EPOLLRDHUP)) {
					int cfd = fd;
					pool.enqueue([cfd, &clients, &fl, epfd]() mutable {
						try {
						   auto it = clients.find(cfd);
						   if (it == clients.end()) return;

						   auto& sock = it->second;
						   auto buffer = sock.receive(8192);

						   if (buffer.empty()) {
						   	throw std::runtime_error("closed");
						   }
						   bool changed = false;

						   {
						    std::lock_guard<std::mutex> lock_ {fl.mu};
						    for (char ch : buffer) {
						    	if (ch == 127 || ch == '\b') {
								if(!fl.content.empty()) {
								   fl.content.pop_back();
								   changed = true;
								}
							} else if (ch >= 32 && ch <= 126) {
								fl.content += ch;
								changed = true;
							}
						        }
						   }
						   if (changed) {
						   	std::string snapshot;
							{
								std::lock_guard<std::mutex> 
									       lock_ {fl.mu};
								snapshot = fl.content;
							}
							for(auto&[oth_fd,oth_sock] : clients){
								try{
								    oth_sock.send(snapshot);
								} catch (...) {}
						        }
						   }
					        }
						catch(...) {
							if(epoll_ctl(epfd, 
								     EPOLL_CTL_DEL, 
								     cfd, nullptr) == 0) {
								clients.erase(cfd);
								std::cout << "'-' client" 
									  << cfd << "\n";
							}
						}
				         });

				}
		        }
		}
	   } catch (const std::exception& e) {
	   	std::cerr << "fatal: " << e.what() << "\n";
		return 1;
	   }
	return 0;
}	
