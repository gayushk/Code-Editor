#include "socket.hpp"
#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <string>
#include <mutex>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <functional>
#include <condition_variable>
#include <sys/epoll.h>
#include <unordered_map>


constexpr int PORT = 8484;
constexpr size_t THREAD_POOL_SIZE = 8;

class ThreadPool {
public:
	explicit ThreadPool(size_t n = THREAD_POOL_SIZE) {
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
	   stop.store(true);
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
	std::atomic<bool> stop{false};
	std::mutex q_mu;
	std::condition_variable cv;	
};

struct ClientMap {
	std::unordered_map<int, SimpleNet::Socket> map;
	std::mutex mu;
};

struct File {
	std::string content;
	std::mutex mu;
};


int main() {
	try{
		File fl;
		ThreadPool pool;
		ClientMap clients;

		SimpleNet::Socket listener;
		listener.bind(PORT);
		listener.listen(32);

		int flags = fcntl(listener.get_fd(), F_GETFL, 0);
		if(flags == -1) throw std::runtime_error("fcntl F_GETFL");
		if(fcntl(listener.get_fd(), F_SETFL, flags | O_NONBLOCK) == -1) 
			throw std::runtime_error("fcntl F_SETFL");

		int epfd = epoll_create1(0);
		if (epfd < 0) throw std::runtime_error("epoll_create");

		struct epoll_event ev{};
		ev.events = EPOLLIN;
		ev.data.fd = listener.get_fd();
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, listener.get_fd(), &ev) < 0) {
			throw std::runtime_error("epoll_ctl");
		}

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
						if(flags == -1) throw std::runtime_error("fcntl F_GETFL client");
						if(fcntl(cfd, F_SETFL, flags | O_NONBLOCK) == -1) {
							throw std::runtime_error("fcntl F_SETFL client");
						}
						ev.events = EPOLLIN | EPOLLRDHUP;
						ev.data.fd = cfd;
						if(epoll_ctl(epfd, 
							     EPOLL_CTL_ADD, 
							     cfd, &ev) < 0) {
							close(fd);
							continue;
						}
						std::string msg;
						{
						  std::lock_guard<std::mutex> lock_fl(fl.mu);
						  msg = fl.content + "\n";
						}
						{
						     std::lock_guard<std::mutex> lock_c(clients.mu);
						     clients.map[cfd] = std::move(client);
						     clients.map[cfd].send(msg);
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
						SimpleNet::RecvResult res;
						   {
						      std::lock_guard<std::mutex> lock_c(clients.mu);
						      auto it = clients.map.find(cfd);
						      if (it == clients.map.end()) return;
						      res = it->second.receive(8192);
						   }
						   if (res.status == SimpleNet::RecvStatus::WouldBlock) {
						   	return;
						   }
						   if(res.status == SimpleNet::RecvStatus::Closed) {
						   	throw std::runtime_error("closed");
						   }
						   bool changed = false;
						   {
						    std::lock_guard<std::mutex> lock_fl {fl.mu};
						    for (char ch : res.data) {
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
									       lock_fl {fl.mu};
								snapshot = fl.content;
							}
							std::lock_guard<std::mutex> lock_c(clients.mu);
							for(auto&[oth_fd,oth_sock] : clients.map){
								try{
								    oth_sock.send(snapshot);
								} catch (...) {}
						        }
						   }
					        }
						catch(...) {
							std::lock_guard<std::mutex> lock_c(clients.mu);
							if(clients.map.erase(cfd)) {
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
