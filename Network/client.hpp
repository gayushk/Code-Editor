#include <unordered_map> 
#include <mutex>
#include <memory>

struct Client {
	SimpleNet::Socket sock;
	std::string recv_buf;
	std::mutex mu; 
};

struct ClientMap {
	std::unordered_map<int, std::shared_ptr<Client>> map;
	std::mutex mu; 
};
