#include <ysu/boost/process/child.hpp>
#include <ysu/lib/threading.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/ysu_node/daemon.hpp>
#include <ysu/node/cli.hpp>
#include <ysu/node/daemonconfig.hpp>
#include <ysu/node/ipc/ipc_server.hpp>
#include <ysu/node/json_handler.hpp>
#include <ysu/node/node.hpp>
#include <ysu/node/openclwork.hpp>
#include <ysu/rpc/rpc.hpp>
#include <ysu/secure/working.hpp>

#include <csignal>
#include <iostream>

namespace
{
void my_abort_signal_handler (int signum)
{
	std::signal (signum, SIG_DFL);
	ysu::dump_crash_stacktrace ();
	ysu::create_load_memory_address_files ();
}
}

namespace
{
volatile sig_atomic_t sig_int_or_term = 0;
}

void ysu_daemon::daemon::run (boost::filesystem::path const & data_path, ysu::node_flags const & flags)
{
	// Override segmentation fault and aborting.
	std::signal (SIGSEGV, &my_abort_signal_handler);
	std::signal (SIGABRT, &my_abort_signal_handler);

	boost::filesystem::create_directories (data_path);
	boost::system::error_code error_chmod;
	ysu::set_secure_perm_directory (data_path, error_chmod);
	std::unique_ptr<ysu::thread_runner> runner;
	ysu::daemon_config config (data_path);
	auto error = ysu::read_node_config_toml (data_path, config, flags.config_overrides);
	ysu::set_use_memory_pools (config.node.use_memory_pools);
	if (!error)
	{
		error = ysu::flags_config_conflicts (flags, config.node);
	}
	if (!error)
	{
		config.node.logging.init (data_path);
		ysu::logger_mt logger{ config.node.logging.min_time_between_log_output };
		boost::asio::io_context io_ctx;
		auto opencl (ysu::opencl_work::create (config.opencl_enable, config.opencl, logger));
		ysu::work_pool opencl_work (config.node.work_threads, config.node.pow_sleep_interval, opencl ? [&opencl](ysu::work_version const version_a, ysu::root const & root_a, uint64_t difficulty_a, std::atomic<int> & ticket_a) {
			return opencl->generate_work (version_a, root_a, difficulty_a, ticket_a);
		}
		                                                                                              : std::function<boost::optional<uint64_t> (ysu::work_version const, ysu::root const &, uint64_t, std::atomic<int> &)> (nullptr));
		ysu::alarm alarm (io_ctx);
		try
		{
			// This avoid a blank prompt during any node initialization delays
			auto initialization_text = "Starting up Ysu node...";
			std::cout << initialization_text << std::endl;
			logger.always_log (initialization_text);

			auto node (std::make_shared<ysu::node> (io_ctx, data_path, alarm, config.node, opencl_work, flags));
			if (!node->init_error ())
			{
				auto network_label = node->network_params.network.get_current_network_as_string ();
				std::cout << "Network: " << network_label << ", version: " << YSU_VERSION_STRING << "\n"
				          << "Path: " << node->application_path.string () << "\n"
				          << "Build Info: " << BUILD_INFO << "\n"
				          << "Database backend: " << node->store.vendor_get () << std::endl;
				auto voting (node->wallets.reps ().voting);
				if (voting > 1)
				{
					std::cout << "Voting with more than one representative can limit performance: " << voting << " representatives are configured" << std::endl;
				}
				node->start ();
				ysu::ipc::ipc_server ipc_server (*node, config.rpc);
#if BOOST_PROCESS_SUPPORTED
				std::unique_ptr<boost::process::child> rpc_process;
				std::unique_ptr<boost::process::child> ysu_pow_server_process;
#endif

				/*if (config.pow_server.enable)
				{
					if (!boost::filesystem::exists (config.pow_server.pow_server_path))
					{
						std::cerr << std::string ("ysu_pow_server is configured to start as a child process, however the file cannot be found at: ") + config.pow_server.pow_server_path << std::endl;
						std::exit (1);
					}

#if BOOST_PROCESS_SUPPORTED
					ysu_pow_server_process = std::make_unique<boost::process::child> (config.pow_server.pow_server_path, "--config_path", data_path / "config-ysu-pow-server.toml");
#else
					std::cerr << "ysu_pow_server is configured to start as a child process, but this is not supported on this system. Disable startup and start the server manually." << std::endl;
					std::exit (1);
#endif
				}*/

				std::unique_ptr<std::thread> rpc_process_thread;
				std::unique_ptr<ysu::rpc> rpc;
				std::unique_ptr<ysu::rpc_handler_interface> rpc_handler;
				if (config.rpc_enable)
				{
					if (!config.rpc.child_process.enable)
					{
						// Launch rpc in-process
						ysu::rpc_config rpc_config;
						auto error = ysu::read_rpc_config_toml (data_path, rpc_config);
						if (error)
						{
							std::cout << error.get_message () << std::endl;
							std::exit (1);
						}
						rpc_handler = std::make_unique<ysu::inprocess_rpc_handler> (*node, ipc_server, config.rpc, [&ipc_server, &alarm, &io_ctx]() {
							ipc_server.stop ();
							alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (3), [&io_ctx]() {
								io_ctx.stop ();
							});
						});
						rpc = ysu::get_rpc (io_ctx, rpc_config, *rpc_handler);
						rpc->start ();
					}
					else
					{
						// Spawn a child rpc process
						if (!boost::filesystem::exists (config.rpc.child_process.rpc_path))
						{
							throw std::runtime_error (std::string ("RPC is configured to spawn a new process however the file cannot be found at: ") + config.rpc.child_process.rpc_path);
						}

						auto network = node->network_params.network.get_current_network_as_string ();
#if BOOST_PROCESS_SUPPORTED
						rpc_process = std::make_unique<boost::process::child> (config.rpc.child_process.rpc_path, "--daemon", "--data_path", data_path, "--network", network);
#else
						auto rpc_exe_command = boost::str (boost::format ("%1% --daemon --data_path=%2% --network=%3%") % config.rpc.child_process.rpc_path % data_path % network);
						rpc_process_thread = std::make_unique<std::thread> ([rpc_exe_command, &logger = node->logger]() {
							ysu::thread_role::set (ysu::thread_role::name::rpc_process_container);
							std::system (rpc_exe_command.c_str ());
							logger.always_log ("RPC server has stopped");
						});
#endif
					}
				}

				debug_assert (!ysu::signal_handler_impl);
				ysu::signal_handler_impl = [&io_ctx]() {
					io_ctx.stop ();
					sig_int_or_term = 1;
				};

				std::signal (SIGINT, &ysu::signal_handler);
				std::signal (SIGTERM, &ysu::signal_handler);

				runner = std::make_unique<ysu::thread_runner> (io_ctx, node->config.io_threads);
				runner->join ();

				if (sig_int_or_term == 1)
				{
					ipc_server.stop ();
					node->stop ();
					if (rpc)
					{
						rpc->stop ();
					}
				}
#if BOOST_PROCESS_SUPPORTED
				if (rpc_process)
				{
					rpc_process->wait ();
				}
#else
				if (rpc_process_thread)
				{
					rpc_process_thread->join ();
				}
#endif
			}
			else
			{
				std::cerr << "Error initializing node\n";
			}
		}
		catch (const std::runtime_error & e)
		{
			std::cerr << "Error while running node (" << e.what () << ")\n";
		}
	}
	else
	{
		std::cerr << "Error deserializing config: " << error.get_message () << std::endl;
	}
}
