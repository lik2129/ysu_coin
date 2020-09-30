#include <ysu/lib/threading.hpp>
#include <ysu/lib/tomlconfig.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/node/common.hpp>
#include <ysu/node/daemonconfig.hpp>
#include <ysu/node/node.hpp>
#include <ysu/node/rocksdb/rocksdb.hpp>
#include <ysu/node/telemetry.hpp>
#include <ysu/node/testing.hpp>
#include <ysu/node/websocket.hpp>
#include <ysu/rpc/rpc.hpp>
#include <ysu/secure/buffer.hpp>

#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <algorithm>
#include <cstdlib>
#include <future>
#include <sstream>

double constexpr ysu::node::price_max;
double constexpr ysu::node::free_cutoff;
size_t constexpr ysu::block_arrival::arrival_size_min;
std::chrono::seconds constexpr ysu::block_arrival::arrival_time_min;

namespace ysu
{
extern unsigned char ysu_bootstrap_weights_live[];
extern size_t ysu_bootstrap_weights_live_size;
extern unsigned char ysu_bootstrap_weights_beta[];
extern size_t ysu_bootstrap_weights_beta_size;
}

void ysu::node::keepalive (std::string const & address_a, uint16_t port_a)
{
	auto node_l (shared_from_this ());
	network.resolver.async_resolve (boost::asio::ip::udp::resolver::query (address_a, std::to_string (port_a)), [node_l, address_a, port_a](boost::system::error_code const & ec, boost::asio::ip::udp::resolver::iterator i_a) {
		if (!ec)
		{
			for (auto i (i_a), n (boost::asio::ip::udp::resolver::iterator{}); i != n; ++i)
			{
				auto endpoint (ysu::transport::map_endpoint_to_v6 (i->endpoint ()));
				std::weak_ptr<ysu::node> node_w (node_l);
				auto channel (node_l->network.find_channel (endpoint));
				if (!channel)
				{
					node_l->network.tcp_channels.start_tcp (endpoint, [node_w](std::shared_ptr<ysu::transport::channel> channel_a) {
						if (auto node_l = node_w.lock ())
						{
							node_l->network.send_keepalive (channel_a);
						}
					});
				}
				else
				{
					node_l->network.send_keepalive (channel);
				}
			}
		}
		else
		{
			node_l->logger.try_log (boost::str (boost::format ("Error resolving address: %1%:%2%: %3%") % address_a % port_a % ec.message ()));
		}
	});
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (rep_crawler & rep_crawler, const std::string & name)
{
	size_t count;
	{
		ysu::lock_guard<std::mutex> guard (rep_crawler.active_mutex);
		count = rep_crawler.active.size ();
	}

	auto sizeof_element = sizeof (decltype (rep_crawler.active)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "active", count, sizeof_element }));
	return composite;
}

ysu::node::node (boost::asio::io_context & io_ctx_a, uint16_t peering_port_a, boost::filesystem::path const & application_path_a, ysu::alarm & alarm_a, ysu::logging const & logging_a, ysu::work_pool & work_a, ysu::node_flags flags_a, unsigned seq) :
node (io_ctx_a, application_path_a, alarm_a, ysu::node_config (peering_port_a, logging_a), work_a, flags_a, seq)
{
}

ysu::node::node (boost::asio::io_context & io_ctx_a, boost::filesystem::path const & application_path_a, ysu::alarm & alarm_a, ysu::node_config const & config_a, ysu::work_pool & work_a, ysu::node_flags flags_a, unsigned seq) :
write_database_queue (!flags_a.force_use_write_database_queue && (config_a.rocksdb_config.enable || ysu::using_rocksdb_in_tests ())),
io_ctx (io_ctx_a),
node_initialized_latch (1),
config (config_a),
stats (config.stat_config),
flags (flags_a),
alarm (alarm_a),
work (work_a),
distributed_work (*this),
logger (config_a.logging.min_time_between_log_output),
store_impl (ysu::make_store (logger, application_path_a, flags.read_only, true, config_a.rocksdb_config, config_a.diagnostics_config.txn_tracking, config_a.block_processor_batch_max_time, config_a.lmdb_config, config_a.backup_before_upgrade, config_a.rocksdb_config.enable)),
store (*store_impl),
wallets_store_impl (std::make_unique<ysu::mdb_wallets_store> (application_path_a / "wallets.ldb", config_a.lmdb_config)),
wallets_store (*wallets_store_impl),
gap_cache (*this),
ledger (store, stats, flags_a.generate_cache, [this]() { this->network.erase_below_version (network_params.protocol.protocol_version_min (true)); }),
checker (config.signature_checker_threads),
network (*this, config.peering_port),
telemetry (std::make_shared<ysu::telemetry> (network, alarm, worker, observers.telemetry, stats, network_params, flags.disable_ongoing_telemetry_requests)),
bootstrap_initiator (*this),
bootstrap (config.peering_port, *this),
application_path (application_path_a),
port_mapping (*this),
vote_processor (checker, active, observers, stats, config, flags, logger, online_reps, ledger, network_params),
rep_crawler (*this),
warmed_up (0),
block_processor (*this, write_database_queue),
// clang-format off
block_processor_thread ([this]() {
	ysu::thread_role::set (ysu::thread_role::name::block_processing);
	this->block_processor.process_blocks ();
}),
// clang-format on
online_reps (ledger, network_params, config.online_weight_minimum.number ()),
vote_uniquer (block_uniquer),
confirmation_height_processor (ledger, write_database_queue, config.conf_height_processor_batch_min_time, config.logging, logger, node_initialized_latch, flags.confirmation_height_processor_mode),
active (*this, confirmation_height_processor),
aggregator (network_params.network, config, stats, active.generator, history, ledger, wallets, active),
payment_observer_processor (observers.blocks),
wallets (wallets_store.init_error (), *this),
startup_time (std::chrono::steady_clock::now ()),
node_seq (seq)
{
	if (!init_error ())
	{
		telemetry->start ();

		if (config.websocket_config.enabled)
		{
			auto endpoint_l (ysu::tcp_endpoint (boost::asio::ip::make_address_v6 (config.websocket_config.address), config.websocket_config.port));
			websocket_server = std::make_shared<ysu::websocket::listener> (logger, wallets, io_ctx, endpoint_l);
			this->websocket_server->run ();
		}

		wallets.observer = [this](bool active) {
			observers.wallet.notify (active);
		};
		network.channel_observer = [this](std::shared_ptr<ysu::transport::channel> channel_a) {
			debug_assert (channel_a != nullptr);
			observers.endpoint.notify (channel_a);
		};
		network.disconnect_observer = [this]() {
			observers.disconnect.notify ();
		};
		if (!config.callback_address.empty ())
		{
			observers.blocks.add ([this](ysu::election_status const & status_a, ysu::account const & account_a, ysu::amount const & amount_a, bool is_state_send_a) {
				auto block_a (status_a.winner);
				if ((status_a.type == ysu::election_status_type::active_confirmed_quorum || status_a.type == ysu::election_status_type::active_confirmation_height) && this->block_arrival.recent (block_a->hash ()))
				{
					auto node_l (shared_from_this ());
					background ([node_l, block_a, account_a, amount_a, is_state_send_a]() {
						boost::property_tree::ptree event;
						event.add ("account", account_a.to_account ());
						event.add ("hash", block_a->hash ().to_string ());
						std::string block_text;
						block_a->serialize_json (block_text);
						event.add ("block", block_text);
						event.add ("amount", amount_a.to_string_dec ());
						if (is_state_send_a)
						{
							event.add ("is_send", is_state_send_a);
							event.add ("subtype", "send");
						}
						// Subtype field
						else if (block_a->type () == ysu::block_type::state)
						{
							if (block_a->link ().is_zero ())
							{
								event.add ("subtype", "change");
							}
							else if (amount_a == 0 && node_l->ledger.is_epoch_link (block_a->link ()))
							{
								event.add ("subtype", "epoch");
							}
							else
							{
								event.add ("subtype", "receive");
							}
						}
						std::stringstream ostream;
						boost::property_tree::write_json (ostream, event);
						ostream.flush ();
						auto body (std::make_shared<std::string> (ostream.str ()));
						auto address (node_l->config.callback_address);
						auto port (node_l->config.callback_port);
						auto target (std::make_shared<std::string> (node_l->config.callback_target));
						auto resolver (std::make_shared<boost::asio::ip::tcp::resolver> (node_l->io_ctx));
						resolver->async_resolve (boost::asio::ip::tcp::resolver::query (address, std::to_string (port)), [node_l, address, port, target, body, resolver](boost::system::error_code const & ec, boost::asio::ip::tcp::resolver::iterator i_a) {
							if (!ec)
							{
								node_l->do_rpc_callback (i_a, address, port, target, body, resolver);
							}
							else
							{
								if (node_l->config.logging.callback_logging ())
								{
									node_l->logger.always_log (boost::str (boost::format ("Error resolving callback: %1%:%2%: %3%") % address % port % ec.message ()));
								}
								node_l->stats.inc (ysu::stat::type::error, ysu::stat::detail::http_callback, ysu::stat::dir::out);
							}
						});
					});
				}
			});
		}
		if (websocket_server)
		{
			observers.blocks.add ([this](ysu::election_status const & status_a, ysu::account const & account_a, ysu::amount const & amount_a, bool is_state_send_a) {
				debug_assert (status_a.type != ysu::election_status_type::ongoing);

				if (this->websocket_server->any_subscriber (ysu::websocket::topic::confirmation))
				{
					auto block_a (status_a.winner);
					std::string subtype;
					if (is_state_send_a)
					{
						subtype = "send";
					}
					else if (block_a->type () == ysu::block_type::state)
					{
						if (block_a->link ().is_zero ())
						{
							subtype = "change";
						}
						else if (amount_a == 0 && this->ledger.is_epoch_link (block_a->link ()))
						{
							subtype = "epoch";
						}
						else
						{
							subtype = "receive";
						}
					}

					this->websocket_server->broadcast_confirmation (block_a, account_a, amount_a, subtype, status_a);
				}
			});

			observers.active_stopped.add ([this](ysu::block_hash const & hash_a) {
				if (this->websocket_server->any_subscriber (ysu::websocket::topic::stopped_election))
				{
					ysu::websocket::message_builder builder;
					this->websocket_server->broadcast (builder.stopped_election (hash_a));
				}
			});

			observers.difficulty.add ([this](uint64_t active_difficulty) {
				if (this->websocket_server->any_subscriber (ysu::websocket::topic::active_difficulty))
				{
					ysu::websocket::message_builder builder;
					auto msg (builder.difficulty_changed (this->default_difficulty (ysu::work_version::work_1), this->default_receive_difficulty (ysu::work_version::work_1), active_difficulty));
					this->websocket_server->broadcast (msg);
				}
			});

			observers.telemetry.add ([this](ysu::telemetry_data const & telemetry_data, ysu::endpoint const & endpoint) {
				if (this->websocket_server->any_subscriber (ysu::websocket::topic::telemetry))
				{
					ysu::websocket::message_builder builder;
					this->websocket_server->broadcast (builder.telemetry_received (telemetry_data, endpoint));
				}
			});
		}
		// Add block confirmation type stats regardless of http-callback and websocket subscriptions
		observers.blocks.add ([this](ysu::election_status const & status_a, ysu::account const & account_a, ysu::amount const & amount_a, bool is_state_send_a) {
			debug_assert (status_a.type != ysu::election_status_type::ongoing);
			switch (status_a.type)
			{
				case ysu::election_status_type::active_confirmed_quorum:
					this->stats.inc (ysu::stat::type::confirmation_observer, ysu::stat::detail::active_quorum, ysu::stat::dir::out);
					break;
				case ysu::election_status_type::active_confirmation_height:
					this->stats.inc (ysu::stat::type::confirmation_observer, ysu::stat::detail::active_conf_height, ysu::stat::dir::out);
					break;
				case ysu::election_status_type::inactive_confirmation_height:
					this->stats.inc (ysu::stat::type::confirmation_observer, ysu::stat::detail::inactive_conf_height, ysu::stat::dir::out);
					break;
				default:
					break;
			}
		});
		observers.endpoint.add ([this](std::shared_ptr<ysu::transport::channel> channel_a) {
			if (channel_a->get_type () == ysu::transport::transport_type::udp)
			{
				this->network.send_keepalive (channel_a);
			}
			else
			{
				this->network.send_keepalive_self (channel_a);
			}
		});
		observers.vote.add ([this](std::shared_ptr<ysu::vote> vote_a, std::shared_ptr<ysu::transport::channel> channel_a, ysu::vote_code code_a) {
			debug_assert (code_a != ysu::vote_code::invalid);
			if (code_a != ysu::vote_code::replay)
			{
				auto active_in_rep_crawler (!this->rep_crawler.response (channel_a, vote_a));
				if (active_in_rep_crawler || code_a == ysu::vote_code::vote)
				{
					// Representative is defined as online if replying to live votes or rep_crawler queries
					this->online_reps.observe (vote_a->account);
				}
			}
			if (code_a == ysu::vote_code::indeterminate)
			{
				this->gap_cache.vote (vote_a);
			}
		});
		if (websocket_server)
		{
			observers.vote.add ([this](std::shared_ptr<ysu::vote> vote_a, std::shared_ptr<ysu::transport::channel> channel_a, ysu::vote_code code_a) {
				if (this->websocket_server->any_subscriber (ysu::websocket::topic::vote))
				{
					ysu::websocket::message_builder builder;
					auto msg (builder.vote_received (vote_a, code_a));
					this->websocket_server->broadcast (msg);
				}
			});
		}
		// Cancelling local work generation
		observers.work_cancel.add ([this](ysu::root const & root_a) {
			this->work.cancel (root_a);
			this->distributed_work.cancel (root_a);
		});

		logger.always_log ("Node starting, version: ", YSU_VERSION_STRING);
		logger.always_log ("Build information: ", BUILD_INFO);
		logger.always_log ("Database backend: ", store.vendor_get ());

		auto network_label = network_params.network.get_current_network_as_string ();
		logger.always_log ("Active network: ", network_label);

		logger.always_log (boost::str (boost::format ("Work pool running %1% threads %2%") % work.threads.size () % (work.opencl ? "(1 for OpenCL)" : "")));
		logger.always_log (boost::str (boost::format ("%1% work peers configured") % config.work_peers.size ()));
		if (!work_generation_enabled ())
		{
			logger.always_log ("Work generation is disabled");
		}

		if (config.logging.node_lifetime_tracing ())
		{
			logger.always_log ("Constructing node");
		}

		logger.always_log (boost::str (boost::format ("Outbound Voting Bandwidth limited to %1% bytes per second, burst ratio %2%") % config.bandwidth_limit % config.bandwidth_limit_burst_ratio));

		// First do a pass with a read to see if any writing needs doing, this saves needing to open a write lock (and potentially blocking)
		auto is_initialized (false);
		{
			auto transaction (store.tx_begin_read ());
			is_initialized = (store.accounts_begin (transaction) != store.accounts_end ());
		}

		ysu::genesis genesis;
		if (!is_initialized && !flags.read_only)
		{
			auto transaction (store.tx_begin_write ({ tables::accounts, tables::blocks, tables::confirmation_height, tables::frontiers }));
			// Store was empty meaning we just created it, add the genesis block
			store.initialize (transaction, genesis, ledger.cache);
		}

		if (!ledger.block_exists (genesis.hash ()))
		{
			std::stringstream ss;
			ss << "Genesis block not found. This commonly indicates a configuration issue, check that the --network or --data_path command line arguments are correct, "
			      "and also the ledger backend node config option. If using a read-only CLI command a ledger must already exist, start the node with --daemon first.";
			if (network_params.network.is_beta_network ())
			{
				ss << " Beta network may have reset, try clearing database files";
			}
			auto str = ss.str ();

			logger.always_log (str);
			std::cerr << str << std::endl;
			std::exit (1);
		}

		if (config.enable_voting)
		{
			std::ostringstream stream;
			stream << "Voting is enabled, more system resources will be used";
			auto voting (wallets.reps ().voting);
			if (voting > 0)
			{
				stream << ". " << voting << " representative(s) are configured";
				if (voting > 1)
				{
					stream << ". Voting with more than one representative can limit performance";
				}
			}
			logger.always_log (stream.str ());
		}

		node_id = ysu::keypair ();
		logger.always_log ("Node ID: ", node_id.pub.to_node_id ());

		if ((network_params.network.is_live_network () || network_params.network.is_beta_network ()) && !flags.inactive_node)
		{
			auto bootstrap_weights = get_bootstrap_weights ();
			// Use bootstrap weights if initial bootstrap is not completed
			bool use_bootstrap_weight = ledger.cache.block_count < bootstrap_weights.first;
			if (use_bootstrap_weight)
			{
				ledger.bootstrap_weights = bootstrap_weights.second;
				for (auto const & rep : ledger.bootstrap_weights)
				{
					logger.always_log ("Using bootstrap rep weight: ", rep.first.to_account (), " -> ", ysu::uint128_union (rep.second).format_balance (Mxrb_ratio, 0, true), " XRB");
				}
			}
			ledger.bootstrap_weight_max_blocks = bootstrap_weights.first;

			// Drop unchecked blocks if initial bootstrap is completed
			if (!flags.disable_unchecked_drop && !use_bootstrap_weight && !flags.read_only)
			{
				auto transaction (store.tx_begin_write ({ tables::unchecked }));
				store.unchecked_clear (transaction);
				logger.always_log ("Dropping unchecked blocks");
			}
		}

		ledger.pruning = flags.enable_pruning || store.pruned_count (store.tx_begin_read ()) > 0;

		if (ledger.pruning)
		{
			if (config.enable_voting && !flags.inactive_node)
			{
				std::string str = "Incompatibility detected between config node.enable_voting and existing pruned blocks";
				logger.always_log (str);
				std::cerr << str << std::endl;
				std::exit (1);
			}
			else if (!flags.enable_pruning && !flags.inactive_node)
			{
				std::string str = "To start node with existing pruned blocks use launch flag --enable_pruning";
				logger.always_log (str);
				std::cerr << str << std::endl;
				std::exit (1);
			}
		}
	}
	node_initialized_latch.count_down ();
}

ysu::node::~node ()
{
	if (config.logging.node_lifetime_tracing ())
	{
		logger.always_log ("Destructing node");
	}
	stop ();
}

void ysu::node::do_rpc_callback (boost::asio::ip::tcp::resolver::iterator i_a, std::string const & address, uint16_t port, std::shared_ptr<std::string> target, std::shared_ptr<std::string> body, std::shared_ptr<boost::asio::ip::tcp::resolver> resolver)
{
	if (i_a != boost::asio::ip::tcp::resolver::iterator{})
	{
		auto node_l (shared_from_this ());
		auto sock (std::make_shared<boost::asio::ip::tcp::socket> (node_l->io_ctx));
		sock->async_connect (i_a->endpoint (), [node_l, target, body, sock, address, port, i_a, resolver](boost::system::error_code const & ec) mutable {
			if (!ec)
			{
				auto req (std::make_shared<boost::beast::http::request<boost::beast::http::string_body>> ());
				req->method (boost::beast::http::verb::post);
				req->target (*target);
				req->version (11);
				req->insert (boost::beast::http::field::host, address);
				req->insert (boost::beast::http::field::content_type, "application/json");
				req->body () = *body;
				req->prepare_payload ();
				boost::beast::http::async_write (*sock, *req, [node_l, sock, address, port, req, i_a, target, body, resolver](boost::system::error_code const & ec, size_t bytes_transferred) mutable {
					if (!ec)
					{
						auto sb (std::make_shared<boost::beast::flat_buffer> ());
						auto resp (std::make_shared<boost::beast::http::response<boost::beast::http::string_body>> ());
						boost::beast::http::async_read (*sock, *sb, *resp, [node_l, sb, resp, sock, address, port, i_a, target, body, resolver](boost::system::error_code const & ec, size_t bytes_transferred) mutable {
							if (!ec)
							{
								if (boost::beast::http::to_status_class (resp->result ()) == boost::beast::http::status_class::successful)
								{
									node_l->stats.inc (ysu::stat::type::http_callback, ysu::stat::detail::initiate, ysu::stat::dir::out);
								}
								else
								{
									if (node_l->config.logging.callback_logging ())
									{
										node_l->logger.try_log (boost::str (boost::format ("Callback to %1%:%2% failed with status: %3%") % address % port % resp->result ()));
									}
									node_l->stats.inc (ysu::stat::type::error, ysu::stat::detail::http_callback, ysu::stat::dir::out);
								}
							}
							else
							{
								if (node_l->config.logging.callback_logging ())
								{
									node_l->logger.try_log (boost::str (boost::format ("Unable complete callback: %1%:%2%: %3%") % address % port % ec.message ()));
								}
								node_l->stats.inc (ysu::stat::type::error, ysu::stat::detail::http_callback, ysu::stat::dir::out);
							};
						});
					}
					else
					{
						if (node_l->config.logging.callback_logging ())
						{
							node_l->logger.try_log (boost::str (boost::format ("Unable to send callback: %1%:%2%: %3%") % address % port % ec.message ()));
						}
						node_l->stats.inc (ysu::stat::type::error, ysu::stat::detail::http_callback, ysu::stat::dir::out);
					}
				});
			}
			else
			{
				if (node_l->config.logging.callback_logging ())
				{
					node_l->logger.try_log (boost::str (boost::format ("Unable to connect to callback address: %1%:%2%: %3%") % address % port % ec.message ()));
				}
				node_l->stats.inc (ysu::stat::type::error, ysu::stat::detail::http_callback, ysu::stat::dir::out);
				++i_a;
				node_l->do_rpc_callback (i_a, address, port, target, body, resolver);
			}
		});
	}
}

bool ysu::node::copy_with_compaction (boost::filesystem::path const & destination)
{
	return store.copy_db (destination);
}

void ysu::node::process_fork (ysu::transaction const & transaction_a, std::shared_ptr<ysu::block> block_a, uint64_t modified_a)
{
	auto root (block_a->root ());
	if (!store.block_exists (transaction_a, block_a->hash ()) && store.root_exists (transaction_a, block_a->root ()))
	{
		std::shared_ptr<ysu::block> ledger_block (ledger.forked_block (transaction_a, *block_a));
		if (ledger_block && !block_confirmed_or_being_confirmed (transaction_a, ledger_block->hash ()) && (ledger.dependents_confirmed (transaction_a, *ledger_block) || modified_a < ysu::seconds_since_epoch () - 300 || !block_arrival.recent (block_a->hash ())))
		{
			std::weak_ptr<ysu::node> this_w (shared_from_this ());
			auto election = active.insert (ledger_block, boost::none, ysu::election_behavior::normal, [this_w, root, root_block_type = block_a->type ()](std::shared_ptr<ysu::block>) {
				if (auto this_l = this_w.lock ())
				{
					auto attempt (this_l->bootstrap_initiator.current_attempt ());
					if (attempt && attempt->mode == ysu::bootstrap_mode::legacy)
					{
						auto transaction (this_l->store.tx_begin_read ());
						ysu::account account{ 0 };
						if (root_block_type == ysu::block_type::receive || root_block_type == ysu::block_type::send || root_block_type == ysu::block_type::change || root_block_type == ysu::block_type::open)
						{
							account = this_l->ledger.store.frontier_get (transaction, root.as_block_hash ());
						}
						if (!account.is_zero ())
						{
							this_l->bootstrap_initiator.connections->requeue_pull (ysu::pull_info (account, root.as_block_hash (), root.as_block_hash (), attempt->incremental_id));
						}
						else if (this_l->ledger.store.account_exists (transaction, root.as_account ()))
						{
							this_l->bootstrap_initiator.connections->requeue_pull (ysu::pull_info (root, ysu::block_hash (0), ysu::block_hash (0), attempt->incremental_id));
						}
					}
				}
			});
			if (election.inserted)
			{
				logger.always_log (boost::str (boost::format ("Resolving fork between our block: %1% and block %2% both with root %3%") % ledger_block->hash ().to_string () % block_a->hash ().to_string () % block_a->root ().to_string ()));
				election.election->transition_active ();
			}
		}
		active.publish (block_a);
	}
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (node & node, const std::string & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (collect_container_info (node.alarm, "alarm"));
	composite->add_component (collect_container_info (node.work, "work"));
	composite->add_component (collect_container_info (node.gap_cache, "gap_cache"));
	composite->add_component (collect_container_info (node.ledger, "ledger"));
	composite->add_component (collect_container_info (node.active, "active"));
	composite->add_component (collect_container_info (node.bootstrap_initiator, "bootstrap_initiator"));
	composite->add_component (collect_container_info (node.bootstrap, "bootstrap"));
	composite->add_component (collect_container_info (node.network, "network"));
	if (node.telemetry)
	{
		composite->add_component (collect_container_info (*node.telemetry, "telemetry"));
	}
	composite->add_component (collect_container_info (node.observers, "observers"));
	composite->add_component (collect_container_info (node.wallets, "wallets"));
	composite->add_component (collect_container_info (node.vote_processor, "vote_processor"));
	composite->add_component (collect_container_info (node.rep_crawler, "rep_crawler"));
	composite->add_component (collect_container_info (node.block_processor, "block_processor"));
	composite->add_component (collect_container_info (node.block_arrival, "block_arrival"));
	composite->add_component (collect_container_info (node.online_reps, "online_reps"));
	composite->add_component (collect_container_info (node.history, "history"));
	composite->add_component (collect_container_info (node.block_uniquer, "block_uniquer"));
	composite->add_component (collect_container_info (node.vote_uniquer, "vote_uniquer"));
	composite->add_component (collect_container_info (node.confirmation_height_processor, "confirmation_height_processor"));
	composite->add_component (collect_container_info (node.worker, "worker"));
	composite->add_component (collect_container_info (node.distributed_work, "distributed_work"));
	composite->add_component (collect_container_info (node.aggregator, "request_aggregator"));
	return composite;
}

void ysu::node::process_active (std::shared_ptr<ysu::block> incoming)
{
	block_arrival.add (incoming->hash ());
	block_processor.add (incoming, ysu::seconds_since_epoch ());
}

ysu::process_return ysu::node::process (ysu::block & block_a)
{
	auto transaction (store.tx_begin_write ({ tables::accounts, tables::blocks, tables::frontiers, tables::pending }, { tables::confirmation_height }));
	auto result (ledger.process (transaction, block_a));
	return result;
}

ysu::process_return ysu::node::process_local (std::shared_ptr<ysu::block> block_a, bool const work_watcher_a)
{
	// Add block hash as recently arrived to trigger automatic rebroadcast and election
	block_arrival.add (block_a->hash ());
	// Set current time to trigger automatic rebroadcast and election
	ysu::unchecked_info info (block_a, block_a->account (), ysu::seconds_since_epoch (), ysu::signature_verification::unknown);
	// Notify block processor to release write lock
	block_processor.wait_write ();
	// Process block
	block_post_events events;
	auto transaction (store.tx_begin_write ({ tables::accounts, tables::blocks, tables::frontiers, tables::pending }, { tables::confirmation_height }));
	return block_processor.process_one (transaction, events, info, work_watcher_a, ysu::block_origin::local);
}

void ysu::node::start ()
{
	long_inactivity_cleanup ();
	network.start ();
	add_initial_peers ();
	if (!flags.disable_legacy_bootstrap)
	{
		ongoing_bootstrap ();
	}
	if (!flags.disable_unchecked_cleanup)
	{
		auto this_l (shared ());
		worker.push_task ([this_l]() {
			this_l->ongoing_unchecked_cleanup ();
		});
	}
	ongoing_store_flush ();
	if (!flags.disable_rep_crawler)
	{
		rep_crawler.start ();
	}
	ongoing_rep_calculation ();
	ongoing_peer_store ();
	ongoing_online_weight_calculation_queue ();
	bool tcp_enabled (false);
	if (config.tcp_incoming_connections_max > 0 && !(flags.disable_bootstrap_listener && flags.disable_tcp_realtime))
	{
		bootstrap.start ();
		tcp_enabled = true;
	}
	if (!flags.disable_backup)
	{
		backup_wallet ();
	}
	if (!flags.disable_search_pending)
	{
		search_pending ();
	}
	if (!flags.disable_wallet_bootstrap)
	{
		// Delay to start wallet lazy bootstrap
		auto this_l (shared ());
		alarm.add (std::chrono::steady_clock::now () + std::chrono::minutes (1), [this_l]() {
			this_l->bootstrap_wallet ();
		});
	}
	// Start port mapping if external address is not defined and TCP or UDP ports are enabled
	if (config.external_address == boost::asio::ip::address_v6{}.any ().to_string () && (tcp_enabled || !flags.disable_udp))
	{
		port_mapping.start ();
	}
}

void ysu::node::stop ()
{
	if (!stopped.exchange (true))
	{
		logger.always_log ("Node stopping");
		// Cancels ongoing work generation tasks, which may be blocking other threads
		// No tasks may wait for work generation in I/O threads, or termination signal capturing will be unable to call node::stop()
		distributed_work.stop ();
		block_processor.stop ();
		if (block_processor_thread.joinable ())
		{
			block_processor_thread.join ();
		}
		aggregator.stop ();
		vote_processor.stop ();
		active.stop ();
		confirmation_height_processor.stop ();
		network.stop ();
		telemetry->stop ();
		if (websocket_server)
		{
			websocket_server->stop ();
		}
		bootstrap_initiator.stop ();
		bootstrap.stop ();
		port_mapping.stop ();
		checker.stop ();
		wallets.stop ();
		stats.stop ();
		worker.stop ();
		auto epoch_upgrade = epoch_upgrading.lock ();
		if (epoch_upgrade->valid ())
		{
			epoch_upgrade->wait ();
		}
		// work pool is not stopped on purpose due to testing setup
	}
}

void ysu::node::keepalive_preconfigured (std::vector<std::string> const & peers_a)
{
	for (auto i (peers_a.begin ()), n (peers_a.end ()); i != n; ++i)
	{
		keepalive (*i, network_params.network.default_node_port);
	}
}

ysu::block_hash ysu::node::latest (ysu::account const & account_a)
{
	auto transaction (store.tx_begin_read ());
	return ledger.latest (transaction, account_a);
}

ysu::uint128_t ysu::node::balance (ysu::account const & account_a)
{
	auto transaction (store.tx_begin_read ());
	return ledger.account_balance (transaction, account_a);
}

std::shared_ptr<ysu::block> ysu::node::block (ysu::block_hash const & hash_a)
{
	auto transaction (store.tx_begin_read ());
	return store.block_get (transaction, hash_a);
}

std::pair<ysu::uint128_t, ysu::uint128_t> ysu::node::balance_pending (ysu::account const & account_a)
{
	std::pair<ysu::uint128_t, ysu::uint128_t> result;
	auto transaction (store.tx_begin_read ());
	result.first = ledger.account_balance (transaction, account_a);
	result.second = ledger.account_pending (transaction, account_a);
	return result;
}

ysu::uint128_t ysu::node::weight (ysu::account const & account_a)
{
	return ledger.weight (account_a);
}

ysu::block_hash ysu::node::rep_block (ysu::account const & account_a)
{
	auto transaction (store.tx_begin_read ());
	ysu::account_info info;
	ysu::block_hash result (0);
	if (!store.account_get (transaction, account_a, info))
	{
		result = ledger.representative (transaction, info.head);
	}
	return result;
}

ysu::uint128_t ysu::node::minimum_principal_weight ()
{
	return minimum_principal_weight (online_reps.online_stake ());
}

ysu::uint128_t ysu::node::minimum_principal_weight (ysu::uint128_t const & online_stake)
{
	return online_stake / network_params.network.principal_weight_factor;
}

void ysu::node::long_inactivity_cleanup ()
{
	bool perform_cleanup = false;
	auto transaction (store.tx_begin_write ({ tables::online_weight, tables::peers }));
	if (store.online_weight_count (transaction) > 0)
	{
		auto i (store.online_weight_begin (transaction));
		auto sample (store.online_weight_begin (transaction));
		auto n (store.online_weight_end ());
		while (++i != n)
		{
			++sample;
		}
		debug_assert (sample != n);
		auto const one_week_ago = static_cast<size_t> ((std::chrono::system_clock::now () - std::chrono::hours (7 * 24)).time_since_epoch ().count ());
		perform_cleanup = sample->first < one_week_ago;
	}
	if (perform_cleanup)
	{
		store.online_weight_clear (transaction);
		store.peer_clear (transaction);
		logger.always_log ("Removed records of peers and online weight after a long period of inactivity");
	}
}

void ysu::node::ongoing_rep_calculation ()
{
	auto now (std::chrono::steady_clock::now ());
	vote_processor.calculate_weights ();
	std::weak_ptr<ysu::node> node_w (shared_from_this ());
	alarm.add (now + std::chrono::minutes (10), [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->ongoing_rep_calculation ();
		}
	});
}

void ysu::node::ongoing_bootstrap ()
{
	auto next_wakeup (network_params.node.bootstrap_interval);
	if (warmed_up < 3)
	{
		// Re-attempt bootstrapping more aggressively on startup
		next_wakeup = std::chrono::seconds (5);
		if (!bootstrap_initiator.in_progress () && !network.empty ())
		{
			++warmed_up;
		}
	}
	bootstrap_initiator.bootstrap ();
	std::weak_ptr<ysu::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + next_wakeup, [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->ongoing_bootstrap ();
		}
	});
}

void ysu::node::ongoing_store_flush ()
{
	{
		auto transaction (store.tx_begin_write ({ tables::vote }));
		store.flush (transaction);
	}
	std::weak_ptr<ysu::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->worker.push_task ([node_l]() {
				node_l->ongoing_store_flush ();
			});
		}
	});
}

void ysu::node::ongoing_peer_store ()
{
	bool stored (network.tcp_channels.store_all (true));
	network.udp_channels.store_all (!stored);
	std::weak_ptr<ysu::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + network_params.node.peer_interval, [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->worker.push_task ([node_l]() {
				node_l->ongoing_peer_store ();
			});
		}
	});
}

void ysu::node::backup_wallet ()
{
	auto transaction (wallets.tx_begin_read ());
	for (auto i (wallets.items.begin ()), n (wallets.items.end ()); i != n; ++i)
	{
		boost::system::error_code error_chmod;
		auto backup_path (application_path / "backup");

		boost::filesystem::create_directories (backup_path);
		ysu::set_secure_perm_directory (backup_path, error_chmod);
		i->second->store.write_backup (transaction, backup_path / (i->first.to_string () + ".json"));
	}
	auto this_l (shared ());
	alarm.add (std::chrono::steady_clock::now () + network_params.node.backup_interval, [this_l]() {
		this_l->backup_wallet ();
	});
}

void ysu::node::search_pending ()
{
	// Reload wallets from disk
	wallets.reload ();
	// Search pending
	wallets.search_pending_all ();
	auto this_l (shared ());
	alarm.add (std::chrono::steady_clock::now () + network_params.node.search_pending_interval, [this_l]() {
		this_l->worker.push_task ([this_l]() {
			this_l->search_pending ();
		});
	});
}

void ysu::node::bootstrap_wallet ()
{
	std::deque<ysu::account> accounts;
	{
		ysu::lock_guard<std::mutex> lock (wallets.mutex);
		auto transaction (wallets.tx_begin_read ());
		for (auto i (wallets.items.begin ()), n (wallets.items.end ()); i != n && accounts.size () < 128; ++i)
		{
			auto & wallet (*i->second);
			ysu::lock_guard<std::recursive_mutex> wallet_lock (wallet.store.mutex);
			for (auto j (wallet.store.begin (transaction)), m (wallet.store.end ()); j != m && accounts.size () < 128; ++j)
			{
				ysu::account account (j->first);
				accounts.push_back (account);
			}
		}
	}
	if (!accounts.empty ())
	{
		bootstrap_initiator.bootstrap_wallet (accounts);
	}
}

void ysu::node::unchecked_cleanup ()
{
	std::vector<ysu::uint128_t> digests;
	std::deque<ysu::unchecked_key> cleaning_list;
	auto attempt (bootstrap_initiator.current_attempt ());
	bool long_attempt (attempt != nullptr && std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now () - attempt->attempt_start).count () > config.unchecked_cutoff_time.count ());
	// Collect old unchecked keys
	if (ledger.cache.block_count >= ledger.bootstrap_weight_max_blocks && !long_attempt)
	{
		auto now (ysu::seconds_since_epoch ());
		auto transaction (store.tx_begin_read ());
		// Max 1M records to clean, max 2 minutes reading to prevent slow i/o systems issues
		for (auto i (store.unchecked_begin (transaction)), n (store.unchecked_end ()); i != n && cleaning_list.size () < 1024 * 1024 && ysu::seconds_since_epoch () - now < 120; ++i)
		{
			ysu::unchecked_key const & key (i->first);
			ysu::unchecked_info const & info (i->second);
			if ((now - info.modified) > static_cast<uint64_t> (config.unchecked_cutoff_time.count ()))
			{
				digests.push_back (network.publish_filter.hash (info.block));
				cleaning_list.push_back (key);
			}
		}
	}
	if (!cleaning_list.empty ())
	{
		logger.always_log (boost::str (boost::format ("Deleting %1% old unchecked blocks") % cleaning_list.size ()));
	}
	// Delete old unchecked keys in batches
	while (!cleaning_list.empty ())
	{
		size_t deleted_count (0);
		auto transaction (store.tx_begin_write ({ tables::unchecked }));
		while (deleted_count++ < 2 * 1024 && !cleaning_list.empty ())
		{
			auto key (cleaning_list.front ());
			cleaning_list.pop_front ();
			if (store.unchecked_exists (transaction, key))
			{
				store.unchecked_del (transaction, key);
			}
		}
	}
	// Delete from the duplicate filter
	network.publish_filter.clear (digests);
}

void ysu::node::ongoing_unchecked_cleanup ()
{
	unchecked_cleanup ();
	auto this_l (shared ());
	alarm.add (std::chrono::steady_clock::now () + network_params.node.unchecked_cleaning_interval, [this_l]() {
		this_l->worker.push_task ([this_l]() {
			this_l->ongoing_unchecked_cleanup ();
		});
	});
}

int ysu::node::price (ysu::uint128_t const & balance_a, int amount_a)
{
	debug_assert (balance_a >= amount_a * ysu::Gxrb_ratio);
	auto balance_l (balance_a);
	double result (0.0);
	for (auto i (0); i < amount_a; ++i)
	{
		balance_l -= ysu::Gxrb_ratio;
		auto balance_scaled ((balance_l / ysu::Mxrb_ratio).convert_to<double> ());
		auto units (balance_scaled / 1000.0);
		auto unit_price (((free_cutoff - units) / free_cutoff) * price_max);
		result += std::min (std::max (0.0, unit_price), price_max);
	}
	return static_cast<int> (result * 100.0);
}

uint64_t ysu::node::default_difficulty (ysu::work_version const version_a) const
{
	uint64_t result{ std::numeric_limits<uint64_t>::max () };
	switch (version_a)
	{
		case ysu::work_version::work_1:
			result = ledger.cache.epoch_2_started ? ysu::work_threshold_base (version_a) : network_params.network.publish_thresholds.epoch_1;
			break;
		default:
			debug_assert (false && "Invalid version specified to default_difficulty");
	}
	return result;
}

uint64_t ysu::node::default_receive_difficulty (ysu::work_version const version_a) const
{
	uint64_t result{ std::numeric_limits<uint64_t>::max () };
	switch (version_a)
	{
		case ysu::work_version::work_1:
			result = ledger.cache.epoch_2_started ? network_params.network.publish_thresholds.epoch_2_receive : network_params.network.publish_thresholds.epoch_1;
			break;
		default:
			debug_assert (false && "Invalid version specified to default_receive_difficulty");
	}
	return result;
}

uint64_t ysu::node::max_work_generate_difficulty (ysu::work_version const version_a) const
{
	return ysu::difficulty::from_multiplier (config.max_work_generate_multiplier, default_difficulty (version_a));
}

bool ysu::node::local_work_generation_enabled () const
{
	return config.work_threads > 0 || work.opencl;
}

bool ysu::node::work_generation_enabled () const
{
	return work_generation_enabled (config.work_peers);
}

bool ysu::node::work_generation_enabled (std::vector<std::pair<std::string, uint16_t>> const & peers_a) const
{
	return !peers_a.empty () || local_work_generation_enabled ();
}

boost::optional<uint64_t> ysu::node::work_generate_blocking (ysu::block & block_a, uint64_t difficulty_a)
{
	auto opt_work_l (work_generate_blocking (block_a.work_version (), block_a.root (), difficulty_a, block_a.account ()));
	if (opt_work_l.is_initialized ())
	{
		block_a.block_work_set (*opt_work_l);
	}
	return opt_work_l;
}

void ysu::node::work_generate (ysu::work_version const version_a, ysu::root const & root_a, uint64_t difficulty_a, std::function<void(boost::optional<uint64_t>)> callback_a, boost::optional<ysu::account> const & account_a, bool secondary_work_peers_a)
{
	auto const & peers_l (secondary_work_peers_a ? config.secondary_work_peers : config.work_peers);
	if (distributed_work.make (version_a, root_a, peers_l, difficulty_a, callback_a, account_a))
	{
		// Error in creating the job (either stopped or work generation is not possible)
		callback_a (boost::none);
	}
}

boost::optional<uint64_t> ysu::node::work_generate_blocking (ysu::work_version const version_a, ysu::root const & root_a, uint64_t difficulty_a, boost::optional<ysu::account> const & account_a)
{
	std::promise<boost::optional<uint64_t>> promise;
	work_generate (
	version_a, root_a, difficulty_a, [&promise](boost::optional<uint64_t> opt_work_a) {
		promise.set_value (opt_work_a);
	},
	account_a);
	return promise.get_future ().get ();
}

boost::optional<uint64_t> ysu::node::work_generate_blocking (ysu::block & block_a)
{
	debug_assert (network_params.network.is_dev_network ());
	return work_generate_blocking (block_a, default_difficulty (ysu::work_version::work_1));
}

boost::optional<uint64_t> ysu::node::work_generate_blocking (ysu::root const & root_a)
{
	debug_assert (network_params.network.is_dev_network ());
	return work_generate_blocking (root_a, default_difficulty (ysu::work_version::work_1));
}

boost::optional<uint64_t> ysu::node::work_generate_blocking (ysu::root const & root_a, uint64_t difficulty_a)
{
	debug_assert (network_params.network.is_dev_network ());
	return work_generate_blocking (ysu::work_version::work_1, root_a, difficulty_a);
}

void ysu::node::add_initial_peers ()
{
	auto transaction (store.tx_begin_read ());
	for (auto i (store.peers_begin (transaction)), n (store.peers_end ()); i != n; ++i)
	{
		ysu::endpoint endpoint (boost::asio::ip::address_v6 (i->first.address_bytes ()), i->first.port ());
		if (!network.reachout (endpoint, config.allow_local_peers))
		{
			std::weak_ptr<ysu::node> node_w (shared_from_this ());
			network.tcp_channels.start_tcp (endpoint, [node_w](std::shared_ptr<ysu::transport::channel> channel_a) {
				if (auto node_l = node_w.lock ())
				{
					node_l->network.send_keepalive (channel_a);
					if (!node_l->flags.disable_rep_crawler)
					{
						node_l->rep_crawler.query (channel_a);
					}
				}
			});
		}
	}
}

void ysu::node::block_confirm (std::shared_ptr<ysu::block> block_a)
{
	auto election = active.insert (block_a);
	if (election.inserted)
	{
		election.election->transition_active ();
	}
}

bool ysu::node::block_confirmed (ysu::block_hash const & hash_a)
{
	auto transaction (store.tx_begin_read ());
	return store.block_exists (transaction, hash_a) && ledger.block_confirmed (transaction, hash_a);
}

bool ysu::node::block_confirmed_or_being_confirmed (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a)
{
	return confirmation_height_processor.is_processing_block (hash_a) || ledger.block_confirmed (transaction_a, hash_a);
}

ysu::uint128_t ysu::node::delta () const
{
	auto result ((online_reps.online_stake () / 100) * config.online_weight_quorum);
	return result;
}

void ysu::node::ongoing_online_weight_calculation_queue ()
{
	std::weak_ptr<ysu::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + (std::chrono::seconds (network_params.node.weight_period)), [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->worker.push_task ([node_l]() {
				node_l->ongoing_online_weight_calculation ();
			});
		}
	});
}

bool ysu::node::online () const
{
	return rep_crawler.total_weight () > (std::max (config.online_weight_minimum.number (), delta ()));
}

void ysu::node::ongoing_online_weight_calculation ()
{
	online_reps.sample ();
	ongoing_online_weight_calculation_queue ();
}

namespace
{
class confirmed_visitor : public ysu::block_visitor
{
public:
	confirmed_visitor (ysu::transaction const & wallet_transaction_a, ysu::transaction const & block_transaction_a, ysu::node & node_a, std::shared_ptr<ysu::block> const & block_a, ysu::block_hash const & hash_a) :
	wallet_transaction (wallet_transaction_a),
	block_transaction (block_transaction_a),
	node (node_a),
	block (block_a),
	hash (hash_a)
	{
	}
	virtual ~confirmed_visitor () = default;
	void scan_receivable (ysu::account const & account_a)
	{
		for (auto const & [id /*unused*/, wallet] : node.wallets.get_wallets ())
		{
			if (wallet->store.exists (wallet_transaction, account_a))
			{
				ysu::account representative;
				ysu::pending_info pending;
				representative = wallet->store.representative (wallet_transaction);
				auto error (node.store.pending_get (block_transaction, ysu::pending_key (account_a, hash), pending));
				if (!error)
				{
					auto node_l (node.shared ());
					auto amount (pending.amount.number ());
					wallet->receive_async (block, representative, amount, [](std::shared_ptr<ysu::block>) {});
				}
				else
				{
					if (!node.store.block_exists (block_transaction, hash))
					{
						node.logger.try_log (boost::str (boost::format ("Confirmed block is missing:  %1%") % hash.to_string ()));
						debug_assert (false && "Confirmed block is missing");
					}
					else
					{
						node.logger.try_log (boost::str (boost::format ("Block %1% has already been received") % hash.to_string ()));
					}
				}
			}
		}
	}
	void state_block (ysu::state_block const & block_a) override
	{
		scan_receivable (block_a.hashables.link.as_account ());
	}
	void send_block (ysu::send_block const & block_a) override
	{
		scan_receivable (block_a.hashables.destination);
	}
	void receive_block (ysu::receive_block const &) override
	{
	}
	void open_block (ysu::open_block const &) override
	{
	}
	void change_block (ysu::change_block const &) override
	{
	}
	ysu::transaction const & wallet_transaction;
	ysu::transaction const & block_transaction;
	ysu::node & node;
	std::shared_ptr<ysu::block> block;
	ysu::block_hash const & hash;
};
}

void ysu::node::receive_confirmed (ysu::transaction const & wallet_transaction_a, ysu::transaction const & block_transaction_a, std::shared_ptr<ysu::block> const & block_a, ysu::block_hash const & hash_a)
{
	confirmed_visitor visitor (wallet_transaction_a, block_transaction_a, *this, block_a, hash_a);
	block_a->visit (visitor);
}

void ysu::node::process_confirmed_data (ysu::transaction const & transaction_a, std::shared_ptr<ysu::block> block_a, ysu::block_hash const & hash_a, ysu::account & account_a, ysu::uint128_t & amount_a, bool & is_state_send_a, ysu::account & pending_account_a)
{
	// Faster account calculation
	account_a = block_a->account ();
	if (account_a.is_zero ())
	{
		account_a = block_a->sideband ().account;
	}
	// Faster amount calculation
	auto previous (block_a->previous ());
	auto previous_balance (ledger.balance (transaction_a, previous));
	auto block_balance (store.block_balance_calculated (block_a));
	if (hash_a != ledger.network_params.ledger.genesis_account)
	{
		amount_a = block_balance > previous_balance ? block_balance - previous_balance : previous_balance - block_balance;
	}
	else
	{
		amount_a = ledger.network_params.ledger.genesis_amount;
	}
	if (auto state = dynamic_cast<ysu::state_block *> (block_a.get ()))
	{
		if (state->hashables.balance < previous_balance)
		{
			is_state_send_a = true;
		}
		pending_account_a = state->hashables.link.as_account ();
	}
	if (auto send = dynamic_cast<ysu::send_block *> (block_a.get ()))
	{
		pending_account_a = send->hashables.destination;
	}
}

void ysu::node::process_confirmed (ysu::election_status const & status_a, uint64_t iteration_a)
{
	auto block_a (status_a.winner);
	auto hash (block_a->hash ());
	const size_t num_iters = (config.block_processor_batch_max_time / network_params.node.process_confirmed_interval) * 4;
	if (ledger.block_exists (hash))
	{
		confirmation_height_processor.add (hash);
	}
	else if (iteration_a < num_iters)
	{
		iteration_a++;
		std::weak_ptr<ysu::node> node_w (shared ());
		alarm.add (std::chrono::steady_clock::now () + network_params.node.process_confirmed_interval, [node_w, status_a, iteration_a]() {
			if (auto node_l = node_w.lock ())
			{
				node_l->process_confirmed (status_a, iteration_a);
			}
		});
	}
	else
	{
		// Do some cleanup due to this block never being processed by confirmation height processor
		active.remove_election_winner_details (hash);
	}
}

bool ysu::block_arrival::add (ysu::block_hash const & hash_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	auto now (std::chrono::steady_clock::now ());
	auto inserted (arrival.get<tag_sequence> ().emplace_back (ysu::block_arrival_info{ now, hash_a }));
	auto result (!inserted.second);
	return result;
}

bool ysu::block_arrival::recent (ysu::block_hash const & hash_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	auto now (std::chrono::steady_clock::now ());
	while (arrival.size () > arrival_size_min && arrival.get<tag_sequence> ().front ().arrival + arrival_time_min < now)
	{
		arrival.get<tag_sequence> ().pop_front ();
	}
	return arrival.get<tag_hash> ().find (hash_a) != arrival.get<tag_hash> ().end ();
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (block_arrival & block_arrival, const std::string & name)
{
	size_t count = 0;
	{
		ysu::lock_guard<std::mutex> guard (block_arrival.mutex);
		count = block_arrival.arrival.size ();
	}

	auto sizeof_element = sizeof (decltype (block_arrival.arrival)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "arrival", count, sizeof_element }));
	return composite;
}

std::shared_ptr<ysu::node> ysu::node::shared ()
{
	return shared_from_this ();
}

int ysu::node::store_version ()
{
	auto transaction (store.tx_begin_read ());
	return store.version_get (transaction);
}

bool ysu::node::init_error () const
{
	return store.init_error () || wallets_store.init_error ();
}

bool ysu::node::epoch_upgrader (ysu::private_key const & prv_a, ysu::epoch epoch_a, uint64_t count_limit, uint64_t threads)
{
	bool error = stopped.load ();
	if (!error)
	{
		auto epoch_upgrade = epoch_upgrading.lock ();
		error = epoch_upgrade->valid () && epoch_upgrade->wait_for (std::chrono::seconds (0)) == std::future_status::timeout;
		if (!error)
		{
			*epoch_upgrade = std::async (std::launch::async, &ysu::node::epoch_upgrader_impl, this, prv_a, epoch_a, count_limit, threads);
		}
	}
	return error;
}

void ysu::node::epoch_upgrader_impl (ysu::private_key const & prv_a, ysu::epoch epoch_a, uint64_t count_limit, uint64_t threads)
{
	ysu::thread_role::set (ysu::thread_role::name::epoch_upgrader);
	auto upgrader_process = [](ysu::node & node_a, std::atomic<uint64_t> & counter, std::shared_ptr<ysu::block> epoch, uint64_t difficulty, ysu::public_key const & signer_a, ysu::root const & root_a, ysu::account const & account_a) {
		epoch->block_work_set (node_a.work_generate_blocking (ysu::work_version::work_1, root_a, difficulty).value_or (0));
		bool valid_signature (!ysu::validate_message (signer_a, epoch->hash (), epoch->block_signature ()));
		bool valid_work (epoch->difficulty () >= difficulty);
		ysu::process_result result (ysu::process_result::old);
		if (valid_signature && valid_work)
		{
			result = node_a.process_local (epoch).code;
		}
		if (result == ysu::process_result::progress)
		{
			++counter;
		}
		else
		{
			bool fork (result == ysu::process_result::fork);
			node_a.logger.always_log (boost::str (boost::format ("Failed to upgrade account %1%. Valid signature: %2%. Valid work: %3%. Block processor fork: %4%") % account_a.to_account () % valid_signature % valid_work % fork));
		}
	};

	uint64_t const upgrade_batch_size = 1000;
	ysu::block_builder builder;
	auto link (ledger.epoch_link (epoch_a));
	ysu::raw_key raw_key;
	raw_key.data = prv_a;
	auto signer (ysu::pub_key (prv_a));
	debug_assert (signer == ledger.epoch_signer (link));

	std::mutex upgrader_mutex;
	ysu::condition_variable upgrader_condition;

	class account_upgrade_item final
	{
	public:
		ysu::account account{ 0 };
		uint64_t modified{ 0 };
	};
	class account_tag
	{
	};
	class modified_tag
	{
	};
	// clang-format off
	boost::multi_index_container<account_upgrade_item,
	boost::multi_index::indexed_by<
		boost::multi_index::ordered_non_unique<boost::multi_index::tag<modified_tag>,
			boost::multi_index::member<account_upgrade_item, uint64_t, &account_upgrade_item::modified>,
			std::greater<uint64_t>>,
		boost::multi_index::hashed_unique<boost::multi_index::tag<account_tag>,
			boost::multi_index::member<account_upgrade_item, ysu::account, &account_upgrade_item::account>>>>
	accounts_list;
	// clang-format on

	bool finished_upgrade (false);

	while (!finished_upgrade && !stopped)
	{
		bool finished_accounts (false);
		uint64_t total_upgraded_accounts (0);
		while (!finished_accounts && count_limit != 0 && !stopped)
		{
			{
				auto transaction (store.tx_begin_read ());
				// Collect accounts to upgrade
				for (auto i (store.accounts_begin (transaction)), n (store.accounts_end ()); i != n && accounts_list.size () < count_limit; ++i)
				{
					ysu::account const & account (i->first);
					ysu::account_info const & info (i->second);
					if (info.epoch () < epoch_a)
					{
						release_assert (ysu::epochs::is_sequential (info.epoch (), epoch_a));
						accounts_list.emplace (account_upgrade_item{ account, info.modified });
					}
				}
			}

			/* Upgrade accounts
			Repeat until accounts with previous epoch exist in latest table */
			std::atomic<uint64_t> upgraded_accounts (0);
			uint64_t workers (0);
			uint64_t attempts (0);
			for (auto i (accounts_list.get<modified_tag> ().begin ()), n (accounts_list.get<modified_tag> ().end ()); i != n && attempts < upgrade_batch_size && attempts < count_limit && !stopped; ++i)
			{
				auto transaction (store.tx_begin_read ());
				ysu::account_info info;
				ysu::account const & account (i->account);
				if (!store.account_get (transaction, account, info) && info.epoch () < epoch_a)
				{
					++attempts;
					auto difficulty (ysu::work_threshold (ysu::work_version::work_1, ysu::block_details (epoch_a, false, false, true)));
					ysu::root const & root (info.head);
					std::shared_ptr<ysu::block> epoch = builder.state ()
					                                     .account (account)
					                                     .previous (info.head)
					                                     .representative (info.representative)
					                                     .balance (info.balance)
					                                     .link (link)
					                                     .sign (raw_key, signer)
					                                     .work (0)
					                                     .build ();
					if (threads != 0)
					{
						{
							ysu::unique_lock<std::mutex> lock (upgrader_mutex);
							++workers;
							while (workers > threads)
							{
								upgrader_condition.wait (lock);
							}
						}
						worker.push_task ([node_l = shared_from_this (), &upgrader_process, &upgrader_mutex, &upgrader_condition, &upgraded_accounts, &workers, epoch, difficulty, signer, root, account]() {
							upgrader_process (*node_l, upgraded_accounts, epoch, difficulty, signer, root, account);
							{
								ysu::lock_guard<std::mutex> lock (upgrader_mutex);
								--workers;
							}
							upgrader_condition.notify_all ();
						});
					}
					else
					{
						upgrader_process (*this, upgraded_accounts, epoch, difficulty, signer, root, account);
					}
				}
			}
			{
				ysu::unique_lock<std::mutex> lock (upgrader_mutex);
				while (workers > 0)
				{
					upgrader_condition.wait (lock);
				}
			}
			total_upgraded_accounts += upgraded_accounts;
			count_limit -= upgraded_accounts;

			if (!accounts_list.empty ())
			{
				logger.always_log (boost::str (boost::format ("%1% accounts were upgraded to new epoch, %2% remain...") % total_upgraded_accounts % (accounts_list.size () - upgraded_accounts)));
				accounts_list.clear ();
			}
			else
			{
				logger.always_log (boost::str (boost::format ("%1% total accounts were upgraded to new epoch") % total_upgraded_accounts));
				finished_accounts = true;
			}
		}

		// Pending blocks upgrade
		bool finished_pending (false);
		uint64_t total_upgraded_pending (0);
		while (!finished_pending && count_limit != 0 && !stopped)
		{
			std::atomic<uint64_t> upgraded_pending (0);
			uint64_t workers (0);
			uint64_t attempts (0);
			auto transaction (store.tx_begin_read ());
			for (auto i (store.pending_begin (transaction, ysu::pending_key (1, 0))), n (store.pending_end ()); i != n && attempts < upgrade_batch_size && attempts < count_limit && !stopped;)
			{
				bool to_next_account (false);
				ysu::pending_key const & key (i->first);
				if (!store.account_exists (transaction, key.account))
				{
					ysu::pending_info const & info (i->second);
					if (info.epoch < epoch_a)
					{
						++attempts;
						release_assert (ysu::epochs::is_sequential (info.epoch, epoch_a));
						auto difficulty (ysu::work_threshold (ysu::work_version::work_1, ysu::block_details (epoch_a, false, false, true)));
						ysu::root const & root (key.account);
						ysu::account const & account (key.account);
						std::shared_ptr<ysu::block> epoch = builder.state ()
						                                     .account (key.account)
						                                     .previous (0)
						                                     .representative (0)
						                                     .balance (0)
						                                     .link (link)
						                                     .sign (raw_key, signer)
						                                     .work (0)
						                                     .build ();
						if (threads != 0)
						{
							{
								ysu::unique_lock<std::mutex> lock (upgrader_mutex);
								++workers;
								while (workers > threads)
								{
									upgrader_condition.wait (lock);
								}
							}
							worker.push_task ([node_l = shared_from_this (), &upgrader_process, &upgrader_mutex, &upgrader_condition, &upgraded_pending, &workers, epoch, difficulty, signer, root, account]() {
								upgrader_process (*node_l, upgraded_pending, epoch, difficulty, signer, root, account);
								{
									ysu::lock_guard<std::mutex> lock (upgrader_mutex);
									--workers;
								}
								upgrader_condition.notify_all ();
							});
						}
						else
						{
							upgrader_process (*this, upgraded_pending, epoch, difficulty, signer, root, account);
						}
					}
				}
				else
				{
					to_next_account = true;
				}
				if (to_next_account)
				{
					// Move to next account if pending account exists or was upgraded
					if (key.account.number () == std::numeric_limits<ysu::uint256_t>::max ())
					{
						break;
					}
					else
					{
						i = store.pending_begin (transaction, ysu::pending_key (key.account.number () + 1, 0));
					}
				}
				else
				{
					// Move to next pending item
					++i;
				}
			}
			{
				ysu::unique_lock<std::mutex> lock (upgrader_mutex);
				while (workers > 0)
				{
					upgrader_condition.wait (lock);
				}
			}

			total_upgraded_pending += upgraded_pending;
			count_limit -= upgraded_pending;

			// Repeat if some pending accounts were upgraded
			if (upgraded_pending != 0)
			{
				logger.always_log (boost::str (boost::format ("%1% unopened accounts with pending blocks were upgraded to new epoch...") % total_upgraded_pending));
			}
			else
			{
				logger.always_log (boost::str (boost::format ("%1% total unopened accounts with pending blocks were upgraded to new epoch") % total_upgraded_pending));
				finished_pending = true;
			}
		}

		finished_upgrade = (total_upgraded_accounts == 0) && (total_upgraded_pending == 0);
	}

	logger.always_log ("Epoch upgrade is completed");
}

std::pair<uint64_t, decltype (ysu::ledger::bootstrap_weights)> ysu::node::get_bootstrap_weights () const
{
	std::unordered_map<ysu::account, ysu::uint128_t> weights;
	const uint8_t * weight_buffer = network_params.network.is_live_network () ? ysu_bootstrap_weights_live : ysu_bootstrap_weights_beta;
	size_t weight_size = network_params.network.is_live_network () ? ysu_bootstrap_weights_live_size : ysu_bootstrap_weights_beta_size;
	ysu::bufferstream weight_stream ((const uint8_t *)weight_buffer, weight_size);
	ysu::uint128_union block_height;
	uint64_t max_blocks = 0;
	if (!ysu::try_read (weight_stream, block_height))
	{
		max_blocks = ysu::narrow_cast<uint64_t> (block_height.number ());
		while (true)
		{
			ysu::account account;
			if (ysu::try_read (weight_stream, account.bytes))
			{
				break;
			}
			ysu::amount weight;
			if (ysu::try_read (weight_stream, weight.bytes))
			{
				break;
			}
			weights[account] = weight.number ();
		}
	}
	return { max_blocks, weights };
}

ysu::inactive_node::inactive_node (boost::filesystem::path const & path_a, ysu::node_flags const & node_flags_a) :
io_context (std::make_shared<boost::asio::io_context> ()),
alarm (*io_context),
work (1)
{
	boost::system::error_code error_chmod;

	/*
	 * @warning May throw a filesystem exception
	 */
	boost::filesystem::create_directories (path_a);
	ysu::set_secure_perm_directory (path_a, error_chmod);
	ysu::daemon_config daemon_config (path_a);
	auto error = ysu::read_node_config_toml (path_a, daemon_config, node_flags_a.config_overrides);
	if (error)
	{
		std::cerr << "Error deserializing config file";
		if (!node_flags_a.config_overrides.empty ())
		{
			std::cerr << " or --config option";
		}
		std::cerr << "\n"
		          << error.get_message () << std::endl;
		std::exit (1);
	}

	auto & node_config = daemon_config.node;
	node_config.peering_port = ysu::get_available_port ();
	node_config.logging.max_size = std::numeric_limits<std::uintmax_t>::max ();
	node_config.logging.init (path_a);

	node = std::make_shared<ysu::node> (*io_context, path_a, alarm, node_config, work, node_flags_a);
	node->active.stop ();
}

ysu::inactive_node::~inactive_node ()
{
	node->stop ();
}

ysu::node_flags const & ysu::inactive_node_flag_defaults ()
{
	static ysu::node_flags node_flags;
	node_flags.inactive_node = true;
	node_flags.read_only = true;
	node_flags.generate_cache.reps = false;
	node_flags.generate_cache.cemented_count = false;
	node_flags.generate_cache.unchecked_count = false;
	node_flags.generate_cache.account_count = false;
	node_flags.generate_cache.epoch_2 = false;
	node_flags.disable_bootstrap_listener = true;
	node_flags.disable_tcp_realtime = true;
	return node_flags;
}

std::unique_ptr<ysu::block_store> ysu::make_store (ysu::logger_mt & logger, boost::filesystem::path const & path, bool read_only, bool add_db_postfix, ysu::rocksdb_config const & rocksdb_config, ysu::txn_tracking_config const & txn_tracking_config_a, std::chrono::milliseconds block_processor_batch_max_time_a, ysu::lmdb_config const & lmdb_config_a, bool backup_before_upgrade, bool use_rocksdb_backend)
{
	if (use_rocksdb_backend || using_rocksdb_in_tests ())
	{
		return std::make_unique<ysu::rocksdb_store> (logger, add_db_postfix ? path / "rocksdb" : path, rocksdb_config, read_only);
	}

	return std::make_unique<ysu::mdb_store> (logger, add_db_postfix ? path / "data.ldb" : path, txn_tracking_config_a, block_processor_batch_max_time_a, lmdb_config_a, backup_before_upgrade);
}
