#include "socket.hpp"
#include "client.hpp"
#include "shared_doc.hpp"
#include "thread_pool.hpp"
#include "autosaver.hpp"
#include "apply_command.hpp"
#include <iostream>
#include <memory>
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
#include <chrono>

constexpr int         PORT              = 8484;
constexpr const char* AUTOSAVE_PATH     = "document.txt";
constexpr int         AUTOSAVE_INTERVAL = 30;

void reset_event(int epfd, int fd, uint32_t events) {
    struct epoll_event ev{};
    ev.events = events | EPOLLONESHOT;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

int main() {
	SharedDoc* doc = shm_create();
	try{
		AutoSaver saver(doc, AUTOSAVE_PATH, AUTOSAVE_INTERVAL);
		ThreadPool pool;
		ClientMap clients;

		SimpleNet::Socket listener;
		listener.bind(PORT);
		listener.listen(32);

		SimpleNet::set_nonblocking(listener.get_fd());

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
						while(true) {
							SimpleNet::Socket client_sock;
							try{client_sock = listener.accept();}
							catch(...) { break; }
							int cfd = client_sock.get_fd();
							SimpleNet::set_nonblocking(cfd);
							auto new_client = std::make_shared<Client>();
							new_client->sock = std::move(client_sock);
							{
								std::lock_guard<std::mutex> lock(clients.mu);
								clients.map[cfd] = new_client;
							}

							ev.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT | EPOLLET;
							ev.data.fd = cfd;
							if(epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
								close(fd);
								continue;
							}

							{
						 		RdLock rd(doc->rwlock);
						 	new_client->sock.send(doc->get() + "\n");
							}
							std::cout << "'+' client" << cfd << "\n";
						}
					} catch (const std::exception& e) {
						std::cerr << "catch error: " 
							  << e.what() << "\n";
					}
				}

				if(events[i].events & (EPOLLIN | EPOLLRDHUP)) {
					int cfd = fd;
					pool.enqueue([cfd, &clients, doc, epfd]() {
						std::shared_ptr<Client> c;
						{
							std::lock_guard<std::mutex> lock(clients.mu);
							if (clients.map.find(cfd) == clients.map.end()) return;
							c = clients.map[cfd];
						}

						// Lock this client during I/O
                    	std::lock_guard<std::mutex> c_lock(c->mu);
						try {
							while(true) {
								auto res =  c->sock.receive(8192);
								if (res.status == SimpleNet::RecvStatus::WouldBlock) { break; }
								if (res.status == SimpleNet::RecvStatus::Closed) { throw std::runtime_error("closed"); }

								c->recv_buf += std::string(res.data.begin(), res.data.end());
							}
							std::string snapshot;
							bool changed = false;

							{
								WrLock wr(doc->rwlock);
								std::string content = doc->get();
								std::string& buf = c->recv_buf;
								size_t start = 0;
								size_t end;
								while ((end = buf.find('\n', start)) != std::string::npos) {
									std::string cmd = buf.substr(start, end - start);
									if (apply_command(cmd, content))
										changed = true;
									start = end + 1;
								}
								buf.erase(0, start);
								if (changed) {
									doc->set(content);
									snapshot = std::move(content);
								}
						   	}

							// Broadcast if changed
							if (changed) {
								std::vector<std::shared_ptr<Client>> targets;
								std::vector<int> fds;
								{
									std::lock_guard<std::mutex> lock(clients.mu);
									for (auto& pair : clients.map) targets.push_back(pair.second);
								}

								for (auto& t : targets) {
									t->sock.send(snapshot);
								}
						   	}

							reset_event(epfd, cfd, EPOLLIN | EPOLLRDHUP);
						}
						catch(...) {
							// Cleanup
							std::lock_guard<std::mutex> lock_c(clients.mu);
							clients.map.erase(cfd);
							epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, nullptr);
							std::cout << "'-' client" << cfd << "\n";
							close(cfd);
						}
					});

				}
		    }
		}
	} catch (const std::exception& e) {
	   	std::cerr << "fatal: " << e.what() << "\n";
		shm_destroy(doc);
		return 1;
	}

	shm_destroy(doc);
	return 0;
}

