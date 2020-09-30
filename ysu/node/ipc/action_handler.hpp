#pragma once

#include <ysu/ipc_flatbuffers_lib/flatbuffer_producer.hpp>
#include <ysu/ipc_flatbuffers_lib/generated/flatbuffers/ysuapi_generated.h>
#include <ysu/node/ipc/ipc_access_config.hpp>

#include <boost/optional.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/minireflect.h>
#include <flatbuffers/registry.h>
#include <flatbuffers/util.h>

namespace ysu
{
class error;
class node;
namespace ipc
{
	class ipc_server;
	class subscriber;

	/**
	 * Implements handlers for the various public IPC messages. When an action handler is completed,
	 * the flatbuffer contains the serialized response object.
	 * @note This is a light-weight class, and an instance can be created for every request.
	 */
	class action_handler final : public flatbuffer_producer, public std::enable_shared_from_this<action_handler>
	{
	public:
		action_handler (ysu::node & node, ysu::ipc::ipc_server & server, std::weak_ptr<ysu::ipc::subscriber> const & subscriber, std::shared_ptr<flatbuffers::FlatBufferBuilder> const & builder);

		void on_account_weight (ysuapi::Envelope const & envelope);
		void on_is_alive (ysuapi::Envelope const & envelope);
		void on_topic_confirmation (ysuapi::Envelope const & envelope);

		/** Request to register a service. The service name is associated with the current session. */
		void on_service_register (ysuapi::Envelope const & envelope);

		/** Request to stop a service by name */
		void on_service_stop (ysuapi::Envelope const & envelope);

		/** Subscribe to the ServiceStop event. The service must first have registered itself on the same session. */
		void on_topic_service_stop (ysuapi::Envelope const & envelope);

		/** Returns a mapping from api message types to handler functions */
		static auto handler_map () -> std::unordered_map<ysuapi::Message, std::function<void(action_handler *, ysuapi::Envelope const &)>, ysu::ipc::enum_hash>;

	private:
		bool has_access (ysuapi::Envelope const & envelope_a, ysu::ipc::access_permission permission_a) const noexcept;
		bool has_access_to_all (ysuapi::Envelope const & envelope_a, std::initializer_list<ysu::ipc::access_permission> permissions_a) const noexcept;
		bool has_access_to_oneof (ysuapi::Envelope const & envelope_a, std::initializer_list<ysu::ipc::access_permission> permissions_a) const noexcept;
		void require (ysuapi::Envelope const & envelope_a, ysu::ipc::access_permission permission_a) const;
		void require_all (ysuapi::Envelope const & envelope_a, std::initializer_list<ysu::ipc::access_permission> permissions_a) const;
		void require_oneof (ysuapi::Envelope const & envelope_a, std::initializer_list<ysu::ipc::access_permission> alternative_permissions_a) const;

		ysu::node & node;
		ysu::ipc::ipc_server & ipc_server;
		std::weak_ptr<ysu::ipc::subscriber> subscriber;
	};
}
}
