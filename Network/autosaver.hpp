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
