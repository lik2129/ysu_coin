#include <ysu/core_test/testutil.hpp>
#include <ysu/node/common.hpp>
#include <ysu/node/testing.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace ysu
{
void force_ysu_dev_network ();
}
namespace
{
std::shared_ptr<ysu::system> system0;
std::shared_ptr<ysu::node> node0;

class fuzz_visitor : public ysu::message_visitor
{
public:
	virtual void keepalive (ysu::keepalive const &) override
	{
	}
	virtual void publish (ysu::publish const &) override
	{
	}
	virtual void confirm_req (ysu::confirm_req const &) override
	{
	}
	virtual void confirm_ack (ysu::confirm_ack const &) override
	{
	}
	virtual void bulk_pull (ysu::bulk_pull const &) override
	{
	}
	virtual void bulk_pull_account (ysu::bulk_pull_account const &) override
	{
	}
	virtual void bulk_push (ysu::bulk_push const &) override
	{
	}
	virtual void frontier_req (ysu::frontier_req const &) override
	{
	}
	virtual void node_id_handshake (ysu::node_id_handshake const &) override
	{
	}
	virtual void telemetry_req (ysu::telemetry_req const &) override
	{
	}
	virtual void telemetry_ack (ysu::telemetry_ack const &) override
	{
	}
};
}

/** Fuzz live message parsing. This covers parsing and block/vote uniquing. */
void fuzz_message_parser (const uint8_t * Data, size_t Size)
{
	static bool initialized = false;
	if (!initialized)
	{
		ysu::force_ysu_dev_network ();
		initialized = true;
		system0 = std::make_shared<ysu::system> (1);
		node0 = system0->nodes[0];
	}

	fuzz_visitor visitor;
	ysu::message_parser parser (node0->network.publish_filter, node0->block_uniquer, node0->vote_uniquer, visitor, node0->work);
	parser.deserialize_buffer (Data, Size);
}

/** Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput (const uint8_t * Data, size_t Size)
{
	fuzz_message_parser (Data, Size);
	return 0;
}
