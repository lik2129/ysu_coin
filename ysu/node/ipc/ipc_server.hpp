#pragma once

#include <ysu/ipc_flatbuffers_lib/generated/flatbuffers/ysuapi_generated.h>
#include <ysu/lib/errors.hpp>
#include <ysu/lib/ipc.hpp>
#include <ysu/node/ipc/ipc_access_config.hpp>
#include <ysu/node/ipc/ipc_broker.hpp>
#include <ysu/node/node_rpc_config.hpp>

#include <atomic>
#include <mutex>

namespace flatbuffers
{
class Parser;
}
namespace ysu
{
class node;
class error;
namespace ipc
{
	class access;
	/** The IPC server accepts connections on one or more configured transports */
	class ipc_server final
	{
	public:
		ipc_server (ysu::node & node, ysu::node_rpc_config const & node_rpc_config);
		~ipc_server ();
		void stop ();

		ysu::node & node;
		ysu::node_rpc_config const & node_rpc_config;

		/** Unique counter/id shared across sessions */
		std::atomic<uint64_t> id_dispenser{ 1 };
		ysu::ipc::broker & get_broker ();
		ysu::ipc::access & get_access ();
		ysu::error reload_access_config ();

	private:
		void setup_callbacks ();
		ysu::ipc::broker broker;
		ysu::ipc::access access;
		std::unique_ptr<dsock_file_remover> file_remover;
		std::vector<std::shared_ptr<ysu::ipc::transport>> transports;
	};
}
}
