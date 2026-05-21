#include <functional>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>

constexpr size_t      THREAD_POOL_SIZE  = 8;

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
