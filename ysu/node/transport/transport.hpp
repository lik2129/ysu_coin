#pragma once

#include <ysu/lib/locks.hpp>
#include <ysu/lib/rate_limiting.hpp>
#include <ysu/lib/stats.hpp>
#include <ysu/node/common.hpp>
#include <ysu/node/socket.hpp>

namespace ysu
{
class bandwidth_limiter final
{
public:
	// initialize with limit 0 = unbounded
	bandwidth_limiter (const double, const size_t);
	bool should_drop (const size_t &);

private:
	ysu::rate::token_bucket bucket;
};

namespace transport
{
	class message;
	ysu::endpoint map_endpoint_to_v6 (ysu::endpoint const &);
	ysu::endpoint map_tcp_to_endpoint (ysu::tcp_endpoint const &);
	ysu::tcp_endpoint map_endpoint_to_tcp (ysu::endpoint const &);
	// Unassigned, reserved, self
	bool reserved_address (ysu::endpoint const &, bool = false);
	static std::chrono::seconds constexpr syn_cookie_cutoff = std::chrono::seconds (5);
	enum class transport_type : uint8_t
	{
		undefined = 0,
		udp = 1,
		tcp = 2,
		loopback = 3
	};
	class channel
	{
	public:
		channel (ysu::node &);
		virtual ~channel () = default;
		virtual size_t hash_code () const = 0;
		virtual bool operator== (ysu::transport::channel const &) const = 0;
		void send (ysu::message const & message_a, std::function<void(boost::system::error_code const &, size_t)> const & callback_a = nullptr, ysu::buffer_drop_policy policy_a = ysu::buffer_drop_policy::limiter);
		virtual void send_buffer (ysu::shared_const_buffer const &, std::function<void(boost::system::error_code const &, size_t)> const & = nullptr, ysu::buffer_drop_policy = ysu::buffer_drop_policy::limiter) = 0;
		virtual std::string to_string () const = 0;
		virtual ysu::endpoint get_endpoint () const = 0;
		virtual ysu::tcp_endpoint get_tcp_endpoint () const = 0;
		virtual ysu::transport::transport_type get_type () const = 0;

		std::chrono::steady_clock::time_point get_last_bootstrap_attempt () const
		{
			ysu::lock_guard<std::mutex> lk (channel_mutex);
			return last_bootstrap_attempt;
		}

		void set_last_bootstrap_attempt (std::chrono::steady_clock::time_point const time_a)
		{
			ysu::lock_guard<std::mutex> lk (channel_mutex);
			last_bootstrap_attempt = time_a;
		}

		std::chrono::steady_clock::time_point get_last_packet_received () const
		{
			ysu::lock_guard<std::mutex> lk (channel_mutex);
			return last_packet_received;
		}

		void set_last_packet_received (std::chrono::steady_clock::time_point const time_a)
		{
			ysu::lock_guard<std::mutex> lk (channel_mutex);
			last_packet_received = time_a;
		}

		std::chrono::steady_clock::time_point get_last_packet_sent () const
		{
			ysu::lock_guard<std::mutex> lk (channel_mutex);
			return last_packet_sent;
		}

		void set_last_packet_sent (std::chrono::steady_clock::time_point const time_a)
		{
			ysu::lock_guard<std::mutex> lk (channel_mutex);
			last_packet_sent = time_a;
		}

		boost::optional<ysu::account> get_node_id_optional () const
		{
			ysu::lock_guard<std::mutex> lk (channel_mutex);
			return node_id;
		}

		ysu::account get_node_id () const
		{
			ysu::lock_guard<std::mutex> lk (channel_mutex);
			if (node_id.is_initialized ())
			{
				return node_id.get ();
			}
			else
			{
				return 0;
			}
		}

		void set_node_id (ysu::account node_id_a)
		{
			ysu::lock_guard<std::mutex> lk (channel_mutex);
			node_id = node_id_a;
		}

		uint8_t get_network_version () const
		{
			return network_version;
		}

		void set_network_version (uint8_t network_version_a)
		{
			network_version = network_version_a;
		}

		mutable std::mutex channel_mutex;

	private:
		std::chrono::steady_clock::time_point last_bootstrap_attempt{ std::chrono::steady_clock::time_point () };
		std::chrono::steady_clock::time_point last_packet_received{ std::chrono::steady_clock::now () };
		std::chrono::steady_clock::time_point last_packet_sent{ std::chrono::steady_clock::now () };
		boost::optional<ysu::account> node_id{ boost::none };
		std::atomic<uint8_t> network_version{ 0 };

	protected:
		ysu::node & node;
	};

	class channel_loopback final : public ysu::transport::channel
	{
	public:
		channel_loopback (ysu::node &);
		size_t hash_code () const override;
		bool operator== (ysu::transport::channel const &) const override;
		void send_buffer (ysu::shared_const_buffer const &, std::function<void(boost::system::error_code const &, size_t)> const & = nullptr, ysu::buffer_drop_policy = ysu::buffer_drop_policy::limiter) override;
		std::string to_string () const override;
		bool operator== (ysu::transport::channel_loopback const & other_a) const
		{
			return endpoint == other_a.get_endpoint ();
		}

		ysu::endpoint get_endpoint () const override
		{
			return endpoint;
		}

		ysu::tcp_endpoint get_tcp_endpoint () const override
		{
			return ysu::transport::map_endpoint_to_tcp (endpoint);
		}

		ysu::transport::transport_type get_type () const override
		{
			return ysu::transport::transport_type::loopback;
		}

	private:
		ysu::endpoint const endpoint;
	};
} // namespace transport
} // namespace ysu

namespace std
{
template <>
struct hash<::ysu::transport::channel>
{
	size_t operator() (::ysu::transport::channel const & channel_a) const
	{
		return channel_a.hash_code ();
	}
};
template <>
struct equal_to<std::reference_wrapper<::ysu::transport::channel const>>
{
	bool operator() (std::reference_wrapper<::ysu::transport::channel const> const & lhs, std::reference_wrapper<::ysu::transport::channel const> const & rhs) const
	{
		return lhs.get () == rhs.get ();
	}
};
}

namespace boost
{
template <>
struct hash<::ysu::transport::channel>
{
	size_t operator() (::ysu::transport::channel const & channel_a) const
	{
		std::hash<::ysu::transport::channel> hash;
		return hash (channel_a);
	}
};
template <>
struct hash<std::reference_wrapper<::ysu::transport::channel const>>
{
	size_t operator() (std::reference_wrapper<::ysu::transport::channel const> const & channel_a) const
	{
		std::hash<::ysu::transport::channel> hash;
		return hash (channel_a.get ());
	}
};
}
