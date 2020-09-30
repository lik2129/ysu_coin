#include <ysu/node/node.hpp>
#include <ysu/node/repcrawler.hpp>

#include <boost/format.hpp>

ysu::rep_crawler::rep_crawler (ysu::node & node_a) :
node (node_a)
{
	if (!node.flags.disable_rep_crawler)
	{
		node.observers.endpoint.add ([this](std::shared_ptr<ysu::transport::channel> channel_a) {
			this->query (channel_a);
		});
	}
}

void ysu::rep_crawler::remove (ysu::block_hash const & hash_a)
{
	ysu::lock_guard<std::mutex> lock (active_mutex);
	active.erase (hash_a);
}

void ysu::rep_crawler::start ()
{
	ongoing_crawl ();
}

void ysu::rep_crawler::validate ()
{
	decltype (responses) responses_l;
	{
		ysu::lock_guard<std::mutex> lock (active_mutex);
		responses_l.swap (responses);
	}
	auto transaction (node.store.tx_begin_read ());
	auto minimum = node.minimum_principal_weight ();
	for (auto const & i : responses_l)
	{
		auto & vote = i.second;
		auto & channel = i.first;
		debug_assert (channel != nullptr);
		ysu::uint128_t rep_weight = node.ledger.weight (vote->account);
		if (rep_weight > minimum && channel->get_type () != ysu::transport::transport_type::loopback)
		{
			auto updated_or_inserted = false;
			ysu::unique_lock<std::mutex> lock (probable_reps_mutex);
			auto existing (probable_reps.find (vote->account));
			if (existing != probable_reps.end ())
			{
				probable_reps.modify (existing, [rep_weight, &updated_or_inserted, &vote, &channel](ysu::representative & info) {
					info.last_response = std::chrono::steady_clock::now ();

					// Update if representative channel was changed
					if (info.channel->get_endpoint () != channel->get_endpoint ())
					{
						debug_assert (info.account == vote->account);
						updated_or_inserted = true;
						info.weight = rep_weight;
						info.channel = channel;
					}
				});
			}
			else
			{
				probable_reps.emplace (ysu::representative (vote->account, rep_weight, channel));
				updated_or_inserted = true;
			}
			lock.unlock ();
			if (updated_or_inserted)
			{
				node.logger.try_log (boost::str (boost::format ("Found a representative at %1%") % channel->to_string ()));
			}
		}
		// This tries to assist rep nodes that have lost track of their highest sequence number by replaying our highest known vote back to them
		// Only do this if the sequence number is significantly different to account for network reordering
		// Amplify attack considerations: We're sending out a confirm_ack in response to a confirm_ack for no net traffic increase
		auto max_vote (node.store.vote_max (transaction, vote));
		if (max_vote->sequence > vote->sequence + 10000)
		{
			ysu::confirm_ack confirm (max_vote);
			channel->send (confirm); // this is non essential traffic as it will be resolicited if not received
		}
	}
}

void ysu::rep_crawler::ongoing_crawl ()
{
	auto now (std::chrono::steady_clock::now ());
	auto total_weight_l (total_weight ());
	cleanup_reps ();
	update_weights ();
	validate ();
	query (get_crawl_targets (total_weight_l));
	auto sufficient_weight (total_weight_l > node.config.online_weight_minimum.number ());
	// If online weight drops below minimum, reach out to preconfigured peers
	if (!sufficient_weight)
	{
		node.keepalive_preconfigured (node.config.preconfigured_peers);
	}
	// Reduce crawl frequency when there's enough total peer weight
	unsigned next_run_ms = node.network_params.network.is_dev_network () ? 100 : sufficient_weight ? 7000 : 3000;
	std::weak_ptr<ysu::node> node_w (node.shared ());
	node.alarm.add (now + std::chrono::milliseconds (next_run_ms), [node_w, this]() {
		if (auto node_l = node_w.lock ())
		{
			this->ongoing_crawl ();
		}
	});
}

std::vector<std::shared_ptr<ysu::transport::channel>> ysu::rep_crawler::get_crawl_targets (ysu::uint128_t total_weight_a)
{
	constexpr size_t conservative_count = 10;
	constexpr size_t aggressive_count = 40;

	// Crawl more aggressively if we lack sufficient total peer weight.
	bool sufficient_weight (total_weight_a > node.config.online_weight_minimum.number ());
	uint16_t required_peer_count = sufficient_weight ? conservative_count : aggressive_count;

	// Add random peers. We do this even if we have enough weight, in order to pick up reps
	// that didn't respond when first observed. If the current total weight isn't sufficient, this
	// will be more aggressive. When the node first starts, the rep container is empty and all
	// endpoints will originate from random peers.
	required_peer_count += required_peer_count / 2;

	// The rest of the endpoints are picked randomly
	auto random_peers (node.network.random_set (required_peer_count, 0, true)); // Include channels with ephemeral remote ports
	std::vector<std::shared_ptr<ysu::transport::channel>> result;
	result.insert (result.end (), random_peers.begin (), random_peers.end ());
	return result;
}

void ysu::rep_crawler::query (std::vector<std::shared_ptr<ysu::transport::channel>> const & channels_a)
{
	auto transaction (node.store.tx_begin_read ());
	auto hash_root (node.ledger.hash_root_random (transaction));
	{
		ysu::lock_guard<std::mutex> lock (active_mutex);
		// Don't send same block multiple times in tests
		if (node.network_params.network.is_dev_network ())
		{
			for (auto i (0); active.count (hash_root.first) != 0 && i < 4; ++i)
			{
				hash_root = node.ledger.hash_root_random (transaction);
			}
		}
		active.insert (hash_root.first);
	}
	if (!channels_a.empty ())
	{
		node.active.erase_recently_confirmed (hash_root.first);
	}
	for (auto i (channels_a.begin ()), n (channels_a.end ()); i != n; ++i)
	{
		debug_assert (*i != nullptr);
		on_rep_request (*i);
		node.network.send_confirm_req (*i, hash_root);
	}

	// A representative must respond with a vote within the deadline
	std::weak_ptr<ysu::node> node_w (node.shared ());
	node.alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [node_w, hash = hash_root.first]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->rep_crawler.remove (hash);
		}
	});
}

void ysu::rep_crawler::query (std::shared_ptr<ysu::transport::channel> const & channel_a)
{
	std::vector<std::shared_ptr<ysu::transport::channel>> peers;
	peers.emplace_back (channel_a);
	query (peers);
}

bool ysu::rep_crawler::is_pr (ysu::transport::channel const & channel_a) const
{
	ysu::lock_guard<std::mutex> lock (probable_reps_mutex);
	auto existing = probable_reps.get<tag_channel_ref> ().find (channel_a);
	bool result = false;
	if (existing != probable_reps.get<tag_channel_ref> ().end ())
	{
		result = existing->weight > node.minimum_principal_weight ();
	}
	return result;
}

bool ysu::rep_crawler::response (std::shared_ptr<ysu::transport::channel> & channel_a, std::shared_ptr<ysu::vote> & vote_a)
{
	bool error = true;
	ysu::lock_guard<std::mutex> lock (active_mutex);
	for (auto i = vote_a->begin (), n = vote_a->end (); i != n; ++i)
	{
		if (active.count (*i) != 0)
		{
			responses.emplace_back (channel_a, vote_a);
			error = false;
			break;
		}
	}
	return error;
}

ysu::uint128_t ysu::rep_crawler::total_weight () const
{
	ysu::lock_guard<std::mutex> lock (probable_reps_mutex);
	ysu::uint128_t result (0);
	for (auto i (probable_reps.get<tag_weight> ().begin ()), n (probable_reps.get<tag_weight> ().end ()); i != n; ++i)
	{
		auto weight (i->weight.number ());
		if (weight > 0)
		{
			result = result + weight;
		}
		else
		{
			break;
		}
	}
	return result;
}

void ysu::rep_crawler::on_rep_request (std::shared_ptr<ysu::transport::channel> channel_a)
{
	ysu::lock_guard<std::mutex> lock (probable_reps_mutex);
	if (channel_a->get_tcp_endpoint ().address () != boost::asio::ip::address_v6::any ())
	{
		probably_rep_t::index<tag_channel_ref>::type & channel_ref_index = probable_reps.get<tag_channel_ref> ();

		// Find and update the timestamp on all reps available on the endpoint (a single host may have multiple reps)
		auto itr_pair = channel_ref_index.equal_range (*channel_a);
		for (; itr_pair.first != itr_pair.second; itr_pair.first++)
		{
			channel_ref_index.modify (itr_pair.first, [](ysu::representative & value_a) {
				value_a.last_request = std::chrono::steady_clock::now ();
			});
		}
	}
}

void ysu::rep_crawler::cleanup_reps ()
{
	std::vector<std::shared_ptr<ysu::transport::channel>> channels;
	{
		// Check known rep channels
		ysu::lock_guard<std::mutex> lock (probable_reps_mutex);
		auto iterator (probable_reps.get<tag_last_request> ().begin ());
		while (iterator != probable_reps.get<tag_last_request> ().end ())
		{
			if (iterator->channel->get_tcp_endpoint ().address () != boost::asio::ip::address_v6::any ())
			{
				channels.push_back (iterator->channel);
				++iterator;
			}
			else
			{
				// Remove reps with closed channels
				iterator = probable_reps.get<tag_last_request> ().erase (iterator);
			}
		}
	}
	// Remove reps with inactive channels
	for (auto const & i : channels)
	{
		bool equal (false);
		if (i->get_type () == ysu::transport::transport_type::tcp)
		{
			auto find_channel (node.network.tcp_channels.find_channel (i->get_tcp_endpoint ()));
			if (find_channel != nullptr && *find_channel == *static_cast<ysu::transport::channel_tcp *> (i.get ()))
			{
				equal = true;
			}
		}
		else if (i->get_type () == ysu::transport::transport_type::udp)
		{
			auto find_channel (node.network.udp_channels.channel (i->get_endpoint ()));
			if (find_channel != nullptr && *find_channel == *static_cast<ysu::transport::channel_udp *> (i.get ()))
			{
				equal = true;
			}
		}
		if (!equal)
		{
			ysu::lock_guard<std::mutex> lock (probable_reps_mutex);
			probable_reps.get<tag_channel_ref> ().erase (*i);
		}
	}
}

void ysu::rep_crawler::update_weights ()
{
	ysu::lock_guard<std::mutex> lock (probable_reps_mutex);
	for (auto i (probable_reps.get<tag_last_request> ().begin ()), n (probable_reps.get<tag_last_request> ().end ()); i != n;)
	{
		auto weight (node.ledger.weight (i->account));
		if (weight > 0)
		{
			if (i->weight.number () != weight)
			{
				probable_reps.get<tag_last_request> ().modify (i, [weight](ysu::representative & info) {
					info.weight = weight;
				});
			}
			++i;
		}
		else
		{
			// Erase non representatives
			i = probable_reps.get<tag_last_request> ().erase (i);
		}
	}
}

std::vector<ysu::representative> ysu::rep_crawler::representatives (size_t count_a, ysu::uint128_t const weight_a, boost::optional<decltype (ysu::protocol_constants::protocol_version)> const & opt_version_min_a)
{
	auto version_min (opt_version_min_a.value_or (node.network_params.protocol.protocol_version_min (node.ledger.cache.epoch_2_started)));
	std::vector<representative> result;
	ysu::lock_guard<std::mutex> lock (probable_reps_mutex);
	for (auto i (probable_reps.get<tag_weight> ().begin ()), n (probable_reps.get<tag_weight> ().end ()); i != n && result.size () < count_a; ++i)
	{
		if (i->weight > weight_a && i->channel->get_network_version () >= version_min)
		{
			result.push_back (*i);
		}
	}
	return result;
}

std::vector<ysu::representative> ysu::rep_crawler::principal_representatives (size_t count_a, boost::optional<decltype (ysu::protocol_constants::protocol_version)> const & opt_version_min_a)
{
	return representatives (count_a, node.minimum_principal_weight (), opt_version_min_a);
}

std::vector<std::shared_ptr<ysu::transport::channel>> ysu::rep_crawler::representative_endpoints (size_t count_a)
{
	std::vector<std::shared_ptr<ysu::transport::channel>> result;
	auto reps (representatives (count_a));
	for (auto const & rep : reps)
	{
		result.push_back (rep.channel);
	}
	return result;
}

/** Total number of representatives */
size_t ysu::rep_crawler::representative_count ()
{
	ysu::lock_guard<std::mutex> lock (probable_reps_mutex);
	return probable_reps.size ();
}
