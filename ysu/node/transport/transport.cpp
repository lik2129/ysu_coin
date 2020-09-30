#include <ysu/node/common.hpp>
#include <ysu/node/node.hpp>
#include <ysu/node/transport/transport.hpp>

#include <boost/format.hpp>

#include <numeric>

namespace
{
class callback_visitor : public ysu::message_visitor
{
public:
	void keepalive (ysu::keepalive const & message_a) override
	{
		result = ysu::stat::detail::keepalive;
	}
	void publish (ysu::publish const & message_a) override
	{
		result = ysu::stat::detail::publish;
	}
	void confirm_req (ysu::confirm_req const & message_a) override
	{
		result = ysu::stat::detail::confirm_req;
	}
	void confirm_ack (ysu::confirm_ack const & message_a) override
	{
		result = ysu::stat::detail::confirm_ack;
	}
	void bulk_pull (ysu::bulk_pull const & message_a) override
	{
		result = ysu::stat::detail::bulk_pull;
	}
	void bulk_pull_account (ysu::bulk_pull_account const & message_a) override
	{
		result = ysu::stat::detail::bulk_pull_account;
	}
	void bulk_push (ysu::bulk_push const & message_a) override
	{
		result = ysu::stat::detail::bulk_push;
	}
	void frontier_req (ysu::frontier_req const & message_a) override
	{
		result = ysu::stat::detail::frontier_req;
	}
	void node_id_handshake (ysu::node_id_handshake const & message_a) override
	{
		result = ysu::stat::detail::node_id_handshake;
	}
	void telemetry_req (ysu::telemetry_req const & message_a) override
	{
		result = ysu::stat::detail::telemetry_req;
	}
	void telemetry_ack (ysu::telemetry_ack const & message_a) override
	{
		result = ysu::stat::detail::telemetry_ack;
	}
	ysu::stat::detail result;
};
}

ysu::endpoint ysu::transport::map_endpoint_to_v6 (ysu::endpoint const & endpoint_a)
{
	auto endpoint_l (endpoint_a);
	if (endpoint_l.address ().is_v4 ())
	{
		endpoint_l = ysu::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint_l.address ().to_v4 ()), endpoint_l.port ());
	}
	return endpoint_l;
}

ysu::endpoint ysu::transport::map_tcp_to_endpoint (ysu::tcp_endpoint const & endpoint_a)
{
	return ysu::endpoint (endpoint_a.address (), endpoint_a.port ());
}

ysu::tcp_endpoint ysu::transport::map_endpoint_to_tcp (ysu::endpoint const & endpoint_a)
{
	return ysu::tcp_endpoint (endpoint_a.address (), endpoint_a.port ());
}

ysu::transport::channel::channel (ysu::node & node_a) :
node (node_a)
{
	set_network_version (node_a.network_params.protocol.protocol_version);
}

void ysu::transport::channel::send (ysu::message const & message_a, std::function<void(boost::system::error_code const &, size_t)> const & callback_a, ysu::buffer_drop_policy drop_policy_a)
{
	callback_visitor visitor;
	message_a.visit (visitor);
	auto buffer (message_a.to_shared_const_buffer (node.ledger.cache.epoch_2_started));
	auto detail (visitor.result);
	auto is_droppable_by_limiter = drop_policy_a == ysu::buffer_drop_policy::limiter;
	auto should_drop (node.network.limiter.should_drop (buffer.size ()));
	if (!is_droppable_by_limiter || !should_drop)
	{
		send_buffer (buffer, callback_a, drop_policy_a);
		node.stats.inc (ysu::stat::type::message, detail, ysu::stat::dir::out);
	}
	else
	{
		if (callback_a)
		{
			node.background ([callback_a]() {
				callback_a (boost::system::errc::make_error_code (boost::system::errc::not_supported), 0);
			});
		}

		node.stats.inc (ysu::stat::type::drop, detail, ysu::stat::dir::out);
		if (node.config.logging.network_packet_logging ())
		{
			auto key = static_cast<uint8_t> (detail) << 8;
			node.logger.always_log (boost::str (boost::format ("%1% of size %2% dropped") % node.stats.detail_to_string (key) % buffer.size ()));
		}
	}
}

ysu::transport::channel_loopback::channel_loopback (ysu::node & node_a) :
channel (node_a), endpoint (node_a.network.endpoint ())
{
	set_node_id (node_a.node_id.pub);
	set_network_version (node_a.network_params.protocol.protocol_version);
}

size_t ysu::transport::channel_loopback::hash_code () const
{
	std::hash<::ysu::endpoint> hash;
	return hash (endpoint);
}

bool ysu::transport::channel_loopback::operator== (ysu::transport::channel const & other_a) const
{
	return endpoint == other_a.get_endpoint ();
}

void ysu::transport::channel_loopback::send_buffer (ysu::shared_const_buffer const & buffer_a, std::function<void(boost::system::error_code const &, size_t)> const & callback_a, ysu::buffer_drop_policy drop_policy_a)
{
	release_assert (false && "sending to a loopback channel is not supported");
}

std::string ysu::transport::channel_loopback::to_string () const
{
	return boost::str (boost::format ("%1%") % endpoint);
}

namespace
{
boost::asio::ip::address_v6 mapped_from_v4_bytes (unsigned long address_a)
{
	return boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (address_a));
}
}

bool ysu::transport::reserved_address (ysu::endpoint const & endpoint_a, bool allow_local_peers)
{
	debug_assert (endpoint_a.address ().is_v6 ());
	auto bytes (endpoint_a.address ().to_v6 ());
	auto result (false);
	static auto const rfc1700_min (mapped_from_v4_bytes (0x00000000ul));
	static auto const rfc1700_max (mapped_from_v4_bytes (0x00fffffful));
	static auto const rfc1918_1_min (mapped_from_v4_bytes (0x0a000000ul));
	static auto const rfc1918_1_max (mapped_from_v4_bytes (0x0afffffful));
	static auto const rfc1918_2_min (mapped_from_v4_bytes (0xac100000ul));
	static auto const rfc1918_2_max (mapped_from_v4_bytes (0xac1ffffful));
	static auto const rfc1918_3_min (mapped_from_v4_bytes (0xc0a80000ul));
	static auto const rfc1918_3_max (mapped_from_v4_bytes (0xc0a8fffful));
	static auto const rfc6598_min (mapped_from_v4_bytes (0x64400000ul));
	static auto const rfc6598_max (mapped_from_v4_bytes (0x647ffffful));
	static auto const rfc5737_1_min (mapped_from_v4_bytes (0xc0000200ul));
	static auto const rfc5737_1_max (mapped_from_v4_bytes (0xc00002fful));
	static auto const rfc5737_2_min (mapped_from_v4_bytes (0xc6336400ul));
	static auto const rfc5737_2_max (mapped_from_v4_bytes (0xc63364fful));
	static auto const rfc5737_3_min (mapped_from_v4_bytes (0xcb007100ul));
	static auto const rfc5737_3_max (mapped_from_v4_bytes (0xcb0071fful));
	static auto const ipv4_multicast_min (mapped_from_v4_bytes (0xe0000000ul));
	static auto const ipv4_multicast_max (mapped_from_v4_bytes (0xeffffffful));
	static auto const rfc6890_min (mapped_from_v4_bytes (0xf0000000ul));
	static auto const rfc6890_max (mapped_from_v4_bytes (0xfffffffful));
	static auto const rfc6666_min (boost::asio::ip::make_address_v6 ("100::"));
	static auto const rfc6666_max (boost::asio::ip::make_address_v6 ("100::ffff:ffff:ffff:ffff"));
	static auto const rfc3849_min (boost::asio::ip::make_address_v6 ("2001:db8::"));
	static auto const rfc3849_max (boost::asio::ip::make_address_v6 ("2001:db8:ffff:ffff:ffff:ffff:ffff:ffff"));
	static auto const rfc4193_min (boost::asio::ip::make_address_v6 ("fc00::"));
	static auto const rfc4193_max (boost::asio::ip::make_address_v6 ("fd00:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
	static auto const ipv6_multicast_min (boost::asio::ip::make_address_v6 ("ff00::"));
	static auto const ipv6_multicast_max (boost::asio::ip::make_address_v6 ("ff00:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
	if (endpoint_a.port () == 0)
	{
		result = true;
	}
	else if (bytes >= rfc1700_min && bytes <= rfc1700_max)
	{
		result = true;
	}
	else if (bytes >= rfc5737_1_min && bytes <= rfc5737_1_max)
	{
		result = true;
	}
	else if (bytes >= rfc5737_2_min && bytes <= rfc5737_2_max)
	{
		result = true;
	}
	else if (bytes >= rfc5737_3_min && bytes <= rfc5737_3_max)
	{
		result = true;
	}
	else if (bytes >= ipv4_multicast_min && bytes <= ipv4_multicast_max)
	{
		result = true;
	}
	else if (bytes >= rfc6890_min && bytes <= rfc6890_max)
	{
		result = true;
	}
	else if (bytes >= rfc6666_min && bytes <= rfc6666_max)
	{
		result = true;
	}
	else if (bytes >= rfc3849_min && bytes <= rfc3849_max)
	{
		result = true;
	}
	else if (bytes >= ipv6_multicast_min && bytes <= ipv6_multicast_max)
	{
		result = true;
	}
	else if (!allow_local_peers)
	{
		if (bytes >= rfc1918_1_min && bytes <= rfc1918_1_max)
		{
			result = true;
		}
		else if (bytes >= rfc1918_2_min && bytes <= rfc1918_2_max)
		{
			result = true;
		}
		else if (bytes >= rfc1918_3_min && bytes <= rfc1918_3_max)
		{
			result = true;
		}
		else if (bytes >= rfc6598_min && bytes <= rfc6598_max)
		{
			result = true;
		}
		else if (bytes >= rfc4193_min && bytes <= rfc4193_max)
		{
			result = true;
		}
	}
	return result;
}

using namespace std::chrono_literals;

ysu::bandwidth_limiter::bandwidth_limiter (const double limit_burst_ratio_a, const size_t limit_a) :
bucket (static_cast<size_t> (limit_a * limit_burst_ratio_a), limit_a)
{
}

bool ysu::bandwidth_limiter::should_drop (const size_t & message_size_a)
{
	return !bucket.try_consume (ysu::narrow_cast<unsigned int> (message_size_a));
}
