#pragma once

#include <ysu/node/common.hpp>
#include <ysu/node/peer_exclusion.hpp>
#include <ysu/node/transport/tcp.hpp>
#include <ysu/node/transport/udp.hpp>
#include <ysu/secure/network_filter.hpp>

#include <boost/thread/thread.hpp>

#include <memory>
#include <queue>
#include <unordered_set>
namespace ysu
{
class channel;
class node;
class stats;
class transaction;
class message_buffer final
{
public:
	uint8_t * buffer{ nullptr };
	size_t size{ 0 };
	ysu::endpoint endpoint;
};
/**
  * A circular buffer for servicing ysu realtime messages.
  * This container follows a producer/consumer model where the operating system is producing data in to
  * buffers which are serviced by internal threads.
  * If buffers are not serviced fast enough they're internally dropped.
  * This container has a maximum space to hold N buffers of M size and will allocate them in round-robin order.
  * All public methods are thread-safe
*/
class message_buffer_manager final
{
public:
	// Stats - Statistics
	// Size - Size of each individual buffer
	// Count - Number of buffers to allocate
	message_buffer_manager (ysu::stat & stats, size_t, size_t);
	// Return a buffer where message data can be put
	// Method will attempt to return the first free buffer
	// If there are no free buffers, an unserviced buffer will be dequeued and returned
	// Function will block if there are no free or unserviced buffers
	// Return nullptr if the container has stopped
	ysu::message_buffer * allocate ();
	// Queue a buffer that has been filled with message data and notify servicing threads
	void enqueue (ysu::message_buffer *);
	// Return a buffer that has been filled with message data
	// Function will block until a buffer has been added
	// Return nullptr if the container has stopped
	ysu::message_buffer * dequeue ();
	// Return a buffer to the freelist after is has been serviced
	void release (ysu::message_buffer *);
	// Stop container and notify waiting threads
	void stop ();

private:
	ysu::stat & stats;
	std::mutex mutex;
	ysu::condition_variable condition;
	boost::circular_buffer<ysu::message_buffer *> free;
	boost::circular_buffer<ysu::message_buffer *> full;
	std::vector<uint8_t> slab;
	std::vector<ysu::message_buffer> entries;
	bool stopped;
};
class tcp_message_manager final
{
public:
	tcp_message_manager (unsigned incoming_connections_max_a);
	void put_message (ysu::tcp_message_item const & item_a);
	ysu::tcp_message_item get_message ();
	// Stop container and notify waiting threads
	void stop ();

private:
	std::mutex mutex;
	ysu::condition_variable producer_condition;
	ysu::condition_variable consumer_condition;
	std::deque<ysu::tcp_message_item> entries;
	unsigned max_entries;
	static unsigned const max_entries_per_connection = 16;
	bool stopped{ false };

	friend class network_tcp_message_manager_Test;
};
/**
  * Node ID cookies for node ID handshakes
*/
class syn_cookies final
{
public:
	syn_cookies (size_t);
	void purge (std::chrono::steady_clock::time_point const &);
	// Returns boost::none if the IP is rate capped on syn cookie requests,
	// or if the endpoint already has a syn cookie query
	boost::optional<ysu::uint256_union> assign (ysu::endpoint const &);
	// Returns false if valid, true if invalid (true on error convention)
	// Also removes the syn cookie from the store if valid
	bool validate (ysu::endpoint const &, ysu::account const &, ysu::signature const &);
	std::unique_ptr<container_info_component> collect_container_info (std::string const &);
	size_t cookies_size ();

private:
	class syn_cookie_info final
	{
	public:
		ysu::uint256_union cookie;
		std::chrono::steady_clock::time_point created_at;
	};
	mutable std::mutex syn_cookie_mutex;
	std::unordered_map<ysu::endpoint, syn_cookie_info> cookies;
	std::unordered_map<boost::asio::ip::address, unsigned> cookies_per_ip;
	size_t max_cookies_per_ip;
};
class network final
{
public:
	network (ysu::node &, uint16_t);
	~network ();
	void start ();
	void stop ();
	void flood_message (ysu::message const &, ysu::buffer_drop_policy const = ysu::buffer_drop_policy::limiter, float const = 1.0f);
	void flood_keepalive (float const scale_a = 1.0f)
	{
		ysu::keepalive message;
		random_fill (message.peers);
		flood_message (message, ysu::buffer_drop_policy::limiter, scale_a);
	}
	void flood_keepalive_self (float const scale_a = 0.5f)
	{
		ysu::keepalive message;
		fill_keepalive_self (message.peers);
		flood_message (message, ysu::buffer_drop_policy::limiter, scale_a);
	}
	void flood_vote (std::shared_ptr<ysu::vote> const &, float scale);
	void flood_vote_pr (std::shared_ptr<ysu::vote> const &);
	// Flood block to all PRs and a random selection of non-PRs
	void flood_block_initial (std::shared_ptr<ysu::block> const &);
	// Flood block to a random selection of peers
	void flood_block (std::shared_ptr<ysu::block> const &, ysu::buffer_drop_policy const = ysu::buffer_drop_policy::limiter);
	void flood_block_many (std::deque<std::shared_ptr<ysu::block>>, std::function<void()> = nullptr, unsigned = broadcast_interval_ms);
	void merge_peers (std::array<ysu::endpoint, 8> const &);
	void merge_peer (ysu::endpoint const &);
	void send_keepalive (std::shared_ptr<ysu::transport::channel>);
	void send_keepalive_self (std::shared_ptr<ysu::transport::channel>);
	void send_node_id_handshake (std::shared_ptr<ysu::transport::channel>, boost::optional<ysu::uint256_union> const & query, boost::optional<ysu::uint256_union> const & respond_to);
	void send_confirm_req (std::shared_ptr<ysu::transport::channel> channel_a, std::pair<ysu::block_hash, ysu::block_hash> const & hash_root_a);
	void broadcast_confirm_req (std::shared_ptr<ysu::block>);
	void broadcast_confirm_req_base (std::shared_ptr<ysu::block>, std::shared_ptr<std::vector<std::shared_ptr<ysu::transport::channel>>>, unsigned, bool = false);
	void broadcast_confirm_req_batched_many (std::unordered_map<std::shared_ptr<ysu::transport::channel>, std::deque<std::pair<ysu::block_hash, ysu::root>>>, std::function<void()> = nullptr, unsigned = broadcast_interval_ms, bool = false);
	void broadcast_confirm_req_many (std::deque<std::pair<std::shared_ptr<ysu::block>, std::shared_ptr<std::vector<std::shared_ptr<ysu::transport::channel>>>>>, std::function<void()> = nullptr, unsigned = broadcast_interval_ms);
	std::shared_ptr<ysu::transport::channel> find_node_id (ysu::account const &);
	std::shared_ptr<ysu::transport::channel> find_channel (ysu::endpoint const &);
	void process_message (ysu::message const &, std::shared_ptr<ysu::transport::channel>);
	bool not_a_peer (ysu::endpoint const &, bool);
	// Should we reach out to this endpoint with a keepalive message
	bool reachout (ysu::endpoint const &, bool = false);
	std::deque<std::shared_ptr<ysu::transport::channel>> list (size_t, uint8_t = 0, bool = true);
	std::deque<std::shared_ptr<ysu::transport::channel>> list_non_pr (size_t);
	// Desired fanout for a given scale
	size_t fanout (float scale = 1.0f) const;
	void random_fill (std::array<ysu::endpoint, 8> &) const;
	void fill_keepalive_self (std::array<ysu::endpoint, 8> &) const;
	// Note: The minimum protocol version is used after the random selection, so number of peers can be less than expected.
	std::unordered_set<std::shared_ptr<ysu::transport::channel>> random_set (size_t, uint8_t = 0, bool = false) const;
	// Get the next peer for attempting a tcp bootstrap connection
	ysu::tcp_endpoint bootstrap_peer (bool = false);
	ysu::endpoint endpoint ();
	void cleanup (std::chrono::steady_clock::time_point const &);
	void ongoing_cleanup ();
	// Node ID cookies cleanup
	ysu::syn_cookies syn_cookies;
	void ongoing_syn_cookie_cleanup ();
	void ongoing_keepalive ();
	size_t size () const;
	float size_sqrt () const;
	bool empty () const;
	void erase (ysu::transport::channel const &);
	void erase_below_version (uint8_t);
	ysu::message_buffer_manager buffer_container;
	boost::asio::ip::udp::resolver resolver;
	std::vector<boost::thread> packet_processing_threads;
	ysu::bandwidth_limiter limiter;
	ysu::peer_exclusion excluded_peers;
	ysu::tcp_message_manager tcp_message_manager;
	ysu::node & node;
	ysu::network_filter publish_filter;
	ysu::transport::udp_channels udp_channels;
	ysu::transport::tcp_channels tcp_channels;
	std::atomic<uint16_t> port{ 0 };
	std::function<void()> disconnect_observer;
	// Called when a new channel is observed
	std::function<void(std::shared_ptr<ysu::transport::channel>)> channel_observer;
	std::atomic<bool> stopped{ false };
	boost::asio::steady_timer cleanup_timer;
	boost::asio::steady_timer cookie_timer;
	boost::asio::steady_timer keepalive_timer;
	static unsigned const broadcast_interval_ms = 10;
	static size_t const buffer_size = 512;
	static size_t const confirm_req_hashes_max = 7;
	static size_t const confirm_ack_hashes_max = 12;
};
std::unique_ptr<container_info_component> collect_container_info (network & network, const std::string & name);
}
