#pragma once

#include <ysu/lib/numbers.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ysu
{
class container_info_component;
class distributed_work;
class node;
class root;
struct work_request;

class distributed_work_factory final
{
public:
	distributed_work_factory (ysu::node &);
	~distributed_work_factory ();
	bool make (ysu::work_version const, ysu::root const &, std::vector<std::pair<std::string, uint16_t>> const &, uint64_t, std::function<void(boost::optional<uint64_t>)> const &, boost::optional<ysu::account> const & = boost::none);
	bool make (std::chrono::seconds const &, ysu::work_request const &);
	void cancel (ysu::root const &);
	void cleanup_finished ();
	void stop ();
	size_t size () const;

private:
	std::unordered_multimap<ysu::root, std::weak_ptr<ysu::distributed_work>> items;

	ysu::node & node;
	mutable std::mutex mutex;
	std::atomic<bool> stopped{ false };

	friend std::unique_ptr<container_info_component> collect_container_info (distributed_work_factory &, const std::string &);
};

std::unique_ptr<container_info_component> collect_container_info (distributed_work_factory & distributed_work, const std::string & name);
}
