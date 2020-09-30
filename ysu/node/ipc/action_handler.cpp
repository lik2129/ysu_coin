#include <ysu/ipc_flatbuffers_lib/generated/flatbuffers/ysuapi_generated.h>
#include <ysu/lib/errors.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/node/ipc/action_handler.hpp>
#include <ysu/node/ipc/ipc_server.hpp>
#include <ysu/node/node.hpp>

#include <iostream>

namespace
{
ysu::account parse_account (std::string const & account, bool & out_is_deprecated_format)
{
	ysu::account result (0);
	if (account.empty ())
	{
		throw ysu::error (ysu::error_common::bad_account_number);
	}
	if (result.decode_account (account))
	{
		throw ysu::error (ysu::error_common::bad_account_number);
	}
	else if (account[3] == '-' || account[4] == '-')
	{
		out_is_deprecated_format = true;
	}

	return result;
}
/** Returns the message as a Flatbuffers ObjectAPI type, managed by a unique_ptr */
template <typename T>
auto get_message (ysuapi::Envelope const & envelope)
{
	auto raw (envelope.message_as<T> ()->UnPack ());
	return std::unique_ptr<typename T::NativeTableType> (raw);
}
}

/**
 * Mapping from message type to handler function.
 * @note This must be updated whenever a new message type is added to the Flatbuffers IDL.
 */
auto ysu::ipc::action_handler::handler_map () -> std::unordered_map<ysuapi::Message, std::function<void(ysu::ipc::action_handler *, ysuapi::Envelope const &)>, ysu::ipc::enum_hash>
{
	static std::unordered_map<ysuapi::Message, std::function<void(ysu::ipc::action_handler *, ysuapi::Envelope const &)>, ysu::ipc::enum_hash> handlers;
	if (handlers.empty ())
	{
		handlers.emplace (ysuapi::Message::Message_IsAlive, &ysu::ipc::action_handler::on_is_alive);
		handlers.emplace (ysuapi::Message::Message_TopicConfirmation, &ysu::ipc::action_handler::on_topic_confirmation);
		handlers.emplace (ysuapi::Message::Message_AccountWeight, &ysu::ipc::action_handler::on_account_weight);
		handlers.emplace (ysuapi::Message::Message_ServiceRegister, &ysu::ipc::action_handler::on_service_register);
		handlers.emplace (ysuapi::Message::Message_ServiceStop, &ysu::ipc::action_handler::on_service_stop);
		handlers.emplace (ysuapi::Message::Message_TopicServiceStop, &ysu::ipc::action_handler::on_topic_service_stop);
	}
	return handlers;
}

ysu::ipc::action_handler::action_handler (ysu::node & node_a, ysu::ipc::ipc_server & server_a, std::weak_ptr<ysu::ipc::subscriber> const & subscriber_a, std::shared_ptr<flatbuffers::FlatBufferBuilder> const & builder_a) :
flatbuffer_producer (builder_a),
node (node_a),
ipc_server (server_a),
subscriber (subscriber_a)
{
}

void ysu::ipc::action_handler::on_topic_confirmation (ysuapi::Envelope const & envelope_a)
{
	auto confirmationTopic (get_message<ysuapi::TopicConfirmation> (envelope_a));
	ipc_server.get_broker ().subscribe (subscriber, std::move (confirmationTopic));
	ysuapi::EventAckT ack;
	create_response (ack);
}

void ysu::ipc::action_handler::on_service_register (ysuapi::Envelope const & envelope_a)
{
	require_oneof (envelope_a, { ysu::ipc::access_permission::api_service_register, ysu::ipc::access_permission::service });
	auto query (get_message<ysuapi::ServiceRegister> (envelope_a));
	ipc_server.get_broker ().service_register (query->service_name, this->subscriber);
	ysuapi::SuccessT success;
	create_response (success);
}

void ysu::ipc::action_handler::on_service_stop (ysuapi::Envelope const & envelope_a)
{
	require_oneof (envelope_a, { ysu::ipc::access_permission::api_service_stop, ysu::ipc::access_permission::service });
	auto query (get_message<ysuapi::ServiceStop> (envelope_a));
	if (query->service_name == "node")
	{
		ipc_server.node.stop ();
	}
	else
	{
		ipc_server.get_broker ().service_stop (query->service_name);
	}
	ysuapi::SuccessT success;
	create_response (success);
}

void ysu::ipc::action_handler::on_topic_service_stop (ysuapi::Envelope const & envelope_a)
{
	auto topic (get_message<ysuapi::TopicServiceStop> (envelope_a));
	ipc_server.get_broker ().subscribe (subscriber, std::move (topic));
	ysuapi::EventAckT ack;
	create_response (ack);
}

void ysu::ipc::action_handler::on_account_weight (ysuapi::Envelope const & envelope_a)
{
	require_oneof (envelope_a, { ysu::ipc::access_permission::api_account_weight, ysu::ipc::access_permission::account_query });
	bool is_deprecated_format{ false };
	auto query (get_message<ysuapi::AccountWeight> (envelope_a));
	auto balance (node.weight (parse_account (query->account, is_deprecated_format)));

	ysuapi::AccountWeightResponseT response;
	response.voting_weight = balance.str ();
	create_response (response);
}

void ysu::ipc::action_handler::on_is_alive (ysuapi::Envelope const & envelope)
{
	ysuapi::IsAliveT alive;
	create_response (alive);
}

bool ysu::ipc::action_handler::has_access (ysuapi::Envelope const & envelope_a, ysu::ipc::access_permission permission_a) const noexcept
{
	// If credentials are missing in the envelope, the default user is used
	std::string credentials;
	if (envelope_a.credentials () != nullptr)
	{
		credentials = envelope_a.credentials ()->str ();
	}

	return ipc_server.get_access ().has_access (credentials, permission_a);
}

bool ysu::ipc::action_handler::has_access_to_all (ysuapi::Envelope const & envelope_a, std::initializer_list<ysu::ipc::access_permission> permissions_a) const noexcept
{
	// If credentials are missing in the envelope, the default user is used
	std::string credentials;
	if (envelope_a.credentials () != nullptr)
	{
		credentials = envelope_a.credentials ()->str ();
	}

	return ipc_server.get_access ().has_access_to_all (credentials, permissions_a);
}

bool ysu::ipc::action_handler::has_access_to_oneof (ysuapi::Envelope const & envelope_a, std::initializer_list<ysu::ipc::access_permission> permissions_a) const noexcept
{
	// If credentials are missing in the envelope, the default user is used
	std::string credentials;
	if (envelope_a.credentials () != nullptr)
	{
		credentials = envelope_a.credentials ()->str ();
	}

	return ipc_server.get_access ().has_access_to_oneof (credentials, permissions_a);
}

void ysu::ipc::action_handler::require (ysuapi::Envelope const & envelope_a, ysu::ipc::access_permission permission_a) const
{
	if (!has_access (envelope_a, permission_a))
	{
		throw ysu::error (ysu::error_common::access_denied);
	}
}

void ysu::ipc::action_handler::require_all (ysuapi::Envelope const & envelope_a, std::initializer_list<ysu::ipc::access_permission> permissions_a) const
{
	if (!has_access_to_all (envelope_a, permissions_a))
	{
		throw ysu::error (ysu::error_common::access_denied);
	}
}

void ysu::ipc::action_handler::require_oneof (ysuapi::Envelope const & envelope_a, std::initializer_list<ysu::ipc::access_permission> permissions_a) const
{
	if (!has_access_to_oneof (envelope_a, permissions_a))
	{
		throw ysu::error (ysu::error_common::access_denied);
	}
}
