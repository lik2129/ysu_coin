#include <ysu/node/testing.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

namespace
{
class dev_visitor : public ysu::message_visitor
{
public:
	void keepalive (ysu::keepalive const &) override
	{
		++keepalive_count;
	}
	void publish (ysu::publish const &) override
	{
		++publish_count;
	}
	void confirm_req (ysu::confirm_req const &) override
	{
		++confirm_req_count;
	}
	void confirm_ack (ysu::confirm_ack const &) override
	{
		++confirm_ack_count;
	}
	void bulk_pull (ysu::bulk_pull const &) override
	{
		ASSERT_FALSE (true);
	}
	void bulk_pull_account (ysu::bulk_pull_account const &) override
	{
		ASSERT_FALSE (true);
	}
	void bulk_push (ysu::bulk_push const &) override
	{
		ASSERT_FALSE (true);
	}
	void frontier_req (ysu::frontier_req const &) override
	{
		ASSERT_FALSE (true);
	}
	void node_id_handshake (ysu::node_id_handshake const &) override
	{
		ASSERT_FALSE (true);
	}
	void telemetry_req (ysu::telemetry_req const &) override
	{
		ASSERT_FALSE (true);
	}
	void telemetry_ack (ysu::telemetry_ack const &) override
	{
		ASSERT_FALSE (true);
	}

	uint64_t keepalive_count{ 0 };
	uint64_t publish_count{ 0 };
	uint64_t confirm_req_count{ 0 };
	uint64_t confirm_ack_count{ 0 };
};
}

TEST (message_parser, exact_confirm_ack_size)
{
	ysu::system system (1);
	dev_visitor visitor;
	ysu::network_filter filter (1);
	ysu::block_uniquer block_uniquer;
	ysu::vote_uniquer vote_uniquer (block_uniquer);
	ysu::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, false);
	auto block (std::make_shared<ysu::send_block> (1, 1, 2, ysu::keypair ().prv, 4, *system.work.generate (ysu::root (1))));
	auto vote (std::make_shared<ysu::vote> (0, ysu::keypair ().prv, 0, std::move (block)));
	ysu::confirm_ack message (vote);
	std::vector<uint8_t> bytes;
	{
		ysu::vectorstream stream (bytes);
		message.serialize (stream, true);
	}
	ASSERT_EQ (0, visitor.confirm_ack_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	auto error (false);
	ysu::bufferstream stream1 (bytes.data (), bytes.size ());
	ysu::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_ack (stream1, header1);
	ASSERT_EQ (1, visitor.confirm_ack_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	bytes.push_back (0);
	ysu::bufferstream stream2 (bytes.data (), bytes.size ());
	ysu::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_ack (stream2, header2);
	ASSERT_EQ (1, visitor.confirm_ack_count);
	ASSERT_NE (parser.status, ysu::message_parser::parse_status::success);
}

TEST (message_parser, exact_confirm_req_size)
{
	ysu::system system (1);
	dev_visitor visitor;
	ysu::network_filter filter (1);
	ysu::block_uniquer block_uniquer;
	ysu::vote_uniquer vote_uniquer (block_uniquer);
	ysu::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, false);
	auto block (std::make_shared<ysu::send_block> (1, 1, 2, ysu::keypair ().prv, 4, *system.work.generate (ysu::root (1))));
	ysu::confirm_req message (std::move (block));
	std::vector<uint8_t> bytes;
	{
		ysu::vectorstream stream (bytes);
		message.serialize (stream, false);
	}
	ASSERT_EQ (0, visitor.confirm_req_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	auto error (false);
	ysu::bufferstream stream1 (bytes.data (), bytes.size ());
	ysu::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_req (stream1, header1);
	ASSERT_EQ (1, visitor.confirm_req_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	bytes.push_back (0);
	ysu::bufferstream stream2 (bytes.data (), bytes.size ());
	ysu::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_req (stream2, header2);
	ASSERT_EQ (1, visitor.confirm_req_count);
	ASSERT_NE (parser.status, ysu::message_parser::parse_status::success);
}

TEST (message_parser, exact_confirm_req_hash_size)
{
	ysu::system system (1);
	dev_visitor visitor;
	ysu::network_filter filter (1);
	ysu::block_uniquer block_uniquer;
	ysu::vote_uniquer vote_uniquer (block_uniquer);
	ysu::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, true);
	ysu::send_block block (1, 1, 2, ysu::keypair ().prv, 4, *system.work.generate (ysu::root (1)));
	ysu::confirm_req message (block.hash (), block.root ());
	std::vector<uint8_t> bytes;
	{
		ysu::vectorstream stream (bytes);
		message.serialize (stream, false);
	}
	ASSERT_EQ (0, visitor.confirm_req_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	auto error (false);
	ysu::bufferstream stream1 (bytes.data (), bytes.size ());
	ysu::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_req (stream1, header1);
	ASSERT_EQ (1, visitor.confirm_req_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	bytes.push_back (0);
	ysu::bufferstream stream2 (bytes.data (), bytes.size ());
	ysu::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_req (stream2, header2);
	ASSERT_EQ (1, visitor.confirm_req_count);
	ASSERT_NE (parser.status, ysu::message_parser::parse_status::success);
}

TEST (message_parser, exact_publish_size)
{
	ysu::system system (1);
	dev_visitor visitor;
	ysu::network_filter filter (1);
	ysu::block_uniquer block_uniquer;
	ysu::vote_uniquer vote_uniquer (block_uniquer);
	ysu::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, true);
	auto block (std::make_shared<ysu::send_block> (1, 1, 2, ysu::keypair ().prv, 4, *system.work.generate (ysu::root (1))));
	ysu::publish message (std::move (block));
	std::vector<uint8_t> bytes;
	{
		ysu::vectorstream stream (bytes);
		message.serialize (stream, false);
	}
	ASSERT_EQ (0, visitor.publish_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	auto error (false);
	ysu::bufferstream stream1 (bytes.data (), bytes.size ());
	ysu::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_publish (stream1, header1);
	ASSERT_EQ (1, visitor.publish_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	bytes.push_back (0);
	ysu::bufferstream stream2 (bytes.data (), bytes.size ());
	ysu::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_publish (stream2, header2);
	ASSERT_EQ (1, visitor.publish_count);
	ASSERT_NE (parser.status, ysu::message_parser::parse_status::success);
}

TEST (message_parser, exact_keepalive_size)
{
	ysu::system system (1);
	dev_visitor visitor;
	ysu::network_filter filter (1);
	ysu::block_uniquer block_uniquer;
	ysu::vote_uniquer vote_uniquer (block_uniquer);
	ysu::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, true);
	ysu::keepalive message;
	std::vector<uint8_t> bytes;
	{
		ysu::vectorstream stream (bytes);
		message.serialize (stream, true);
	}
	ASSERT_EQ (0, visitor.keepalive_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	auto error (false);
	ysu::bufferstream stream1 (bytes.data (), bytes.size ());
	ysu::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_keepalive (stream1, header1);
	ASSERT_EQ (1, visitor.keepalive_count);
	ASSERT_EQ (parser.status, ysu::message_parser::parse_status::success);
	bytes.push_back (0);
	ysu::bufferstream stream2 (bytes.data (), bytes.size ());
	ysu::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_keepalive (stream2, header2);
	ASSERT_EQ (1, visitor.keepalive_count);
	ASSERT_NE (parser.status, ysu::message_parser::parse_status::success);
}
