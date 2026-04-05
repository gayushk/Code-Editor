#include "socket.hpp"
#include "shared_doc.hpp"
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
#include <chrono>

constexpr int         PORT              = 8484;
constexpr size_t      THREAD_POOL_SIZE  = 8;
constexpr const char* AUTOSAVE_PATH     = "document.txt";
constexpr int         AUTOSAVE_INTERVAL = 30;

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

class AutoSaver {
public:
	AutoSaver(SharedDoc* doc, const char* path, int interval_s)
		: doc_(doc), path_(path), interval_(interval_s)
	{
		thread_ = std::thread([this] { run(); });
	}

	~AutoSaver() {
		{
			std::lock_guard<std::mutex> lk(mu_);
			stop_ = true;
		}
		cv_.notify_one();
		thread_.join();
	}

private:
	void run() {
		std::unique_lock<std::mutex> lk(mu_);
		while (!stop_) {
			cv_.wait_for(lk, interval_, [this] { return stop_; });
			if (stop_) break;
			lk.unlock();
			save();
			lk.lock();
		}
	}

	void save() {
		std::string content;
		{
			RdLock rd(doc_->rwlock);
			content = doc_->get();
		}
		std::string tmp = path_ + ".tmp";
		int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) { std::cerr << "autosave: open failed\n"; return; }
		size_t written = 0;
		while (written < content.size()) {
			ssize_t n = ::write(fd, content.data() + written, content.size() - written);
			if (n < 0) { close(fd); std::cerr << "autosave: write failed\n"; return; }
			written += static_cast<size_t>(n);
		}
		close(fd);
		if (rename(tmp.c_str(), path_.c_str()) < 0)
			std::cerr << "autosave: rename failed\n";
		else
			std::cout << "autosave: " << content.size() << " bytes -> " << path_ << "\n";
	}

	SharedDoc*                      doc_;
	std::string                     path_;
	std::chrono::seconds            interval_;
	std::thread                     thread_;
	std::mutex                      mu_;
	std::condition_variable         cv_;
	bool                            stop_ = false;
};

struct ClientMap {
	std::unordered_map<int, SimpleNet::Socket> map;
	std::mutex mu;
};


static bool apply_command(const std::string& cmd, std::string& content) {
	if (cmd.size() < 3) return false;
	try {
		if (cmd[0] == 'I' && cmd[1] == ':') {
			size_t sep = cmd.find(':', 2);
			if (sep == std::string::npos) return false;
			int pos = std::stoi(cmd.substr(2, sep - 2));
			if (cmd.size() <= sep + 1) return false;
			char ch = cmd[sep + 1];
			if (pos < 0) pos = 0;
			if (pos > (int)content.size()) pos = (int)content.size();
			content.insert(pos, 1, ch);
			return true;
		}
		if (cmd[0] == 'D' && cmd[1] == ':') {
			int pos = std::stoi(cmd.substr(2));
			if (pos < 0 || pos >= (int)content.size()) return false;
			content.erase(pos, 1);
			return true;
		}
	} catch (const std::exception&) { return false; }
	return false;
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
						auto client = listener.accept();
						int cfd = client.get_fd();
						SimpleNet::set_nonblocking(cfd);
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
						  RdLock rd(doc->rwlock);
						  msg = doc->get() + "\n";
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
					pool.enqueue([cfd, &clients, doc, epfd]() mutable {
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
						   std::string incoming(res.data.begin(), res.data.end());
						   bool changed = false;
						   std::string snapshot;
						   {
						    WrLock wr(doc->rwlock);
						    std::string content = doc->get();
						    size_t start = 0;
						    size_t end;
						    while ((end = incoming.find('\n', start)) != std::string::npos) {
							std::string cmd = incoming.substr(start, end - start);
							if (apply_command(cmd, content))
								changed = true;
							start = end + 1;
						    }
						    if (changed) {
							doc->set(content);
							snapshot = std::move(content);
						    }
						   }
						   if (changed) {
							std::vector<int> fds;
							{
								std::lock_guard<std::mutex> lock_c(clients.mu);
								fds.reserve(clients.map.size());
								for (auto& [fd, _] : clients.map) fds.push_back(fd);
							}
							for (int fd : fds) {
								std::lock_guard<std::mutex> lock_c(clients.mu);
								auto it = clients.map.find(fd);
								if (it != clients.map.end())
									try { it->second.send(snapshot); } catch (...) {}
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
		shm_destroy(doc);
		return 1;
	   }
	shm_destroy(doc);
	return 0;
}

