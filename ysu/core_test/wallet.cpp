#include <ysu/crypto_lib/random_pool.hpp>
#include <ysu/lib/threading.hpp>
#include <ysu/node/lmdb/wallet_value.hpp>
#include <ysu/node/testing.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/filesystem.hpp>

using namespace std::chrono_literals;
unsigned constexpr ysu::wallet_store::version_current;

TEST (wallet, no_special_keys_accounts)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (init);
	ysu::keypair key1;
	ASSERT_FALSE (wallet.exists (transaction, key1.pub));
	wallet.insert_adhoc (transaction, key1.prv);
	ASSERT_TRUE (wallet.exists (transaction, key1.pub));

	for (uint64_t account = 0; account < ysu::wallet_store::special_count; account++)
	{
		ysu::account account_l (account);
		ASSERT_FALSE (wallet.exists (transaction, account_l));
	}
}

TEST (wallet, no_key)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (init);
	ysu::keypair key1;
	ysu::raw_key prv1;
	ASSERT_TRUE (wallet.fetch (transaction, key1.pub, prv1));
	ASSERT_TRUE (wallet.valid_password (transaction));
}

TEST (wallet, fetch_locked)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_TRUE (wallet.valid_password (transaction));
	ysu::keypair key1;
	ASSERT_EQ (key1.pub, wallet.insert_adhoc (transaction, key1.prv));
	auto key2 (wallet.deterministic_insert (transaction));
	ASSERT_FALSE (key2.is_zero ());
	ysu::raw_key key3;
	key3.data = 1;
	wallet.password.value_set (key3);
	ASSERT_FALSE (wallet.valid_password (transaction));
	ysu::raw_key key4;
	ASSERT_TRUE (wallet.fetch (transaction, key1.pub, key4));
	ASSERT_TRUE (wallet.fetch (transaction, key2, key4));
}

TEST (wallet, retrieval)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (init);
	ysu::keypair key1;
	ASSERT_TRUE (wallet.valid_password (transaction));
	wallet.insert_adhoc (transaction, key1.prv);
	ysu::raw_key prv1;
	ASSERT_FALSE (wallet.fetch (transaction, key1.pub, prv1));
	ASSERT_TRUE (wallet.valid_password (transaction));
	ASSERT_EQ (key1.prv, prv1);
	wallet.password.values[0]->bytes[16] ^= 1;
	ysu::raw_key prv2;
	ASSERT_TRUE (wallet.fetch (transaction, key1.pub, prv2));
	ASSERT_FALSE (wallet.valid_password (transaction));
}

TEST (wallet, empty_iteration)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (init);
	auto i (wallet.begin (transaction));
	auto j (wallet.end ());
	ASSERT_EQ (i, j);
}

TEST (wallet, one_item_iteration)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (init);
	ysu::keypair key1;
	wallet.insert_adhoc (transaction, key1.prv);
	for (auto i (wallet.begin (transaction)), j (wallet.end ()); i != j; ++i)
	{
		ASSERT_EQ (key1.pub, ysu::uint256_union (i->first));
		ysu::raw_key password;
		wallet.wallet_key (password, transaction);
		ysu::raw_key key;
		key.decrypt (ysu::wallet_value (i->second).key, password, (ysu::uint256_union (i->first)).owords[0].number ());
		ASSERT_EQ (key1.prv, key);
	}
}

TEST (wallet, two_item_iteration)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	ysu::keypair key1;
	ysu::keypair key2;
	ASSERT_NE (key1.pub, key2.pub);
	std::unordered_set<ysu::public_key> pubs;
	std::unordered_set<ysu::private_key> prvs;
	ysu::kdf kdf;
	{
		auto transaction (env.tx_begin_write ());
		ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
		ASSERT_FALSE (init);
		wallet.insert_adhoc (transaction, key1.prv);
		wallet.insert_adhoc (transaction, key2.prv);
		for (auto i (wallet.begin (transaction)), j (wallet.end ()); i != j; ++i)
		{
			pubs.insert (i->first);
			ysu::raw_key password;
			wallet.wallet_key (password, transaction);
			ysu::raw_key key;
			key.decrypt (ysu::wallet_value (i->second).key, password, (i->first).owords[0].number ());
			prvs.insert (key.as_private_key ());
		}
	}
	ASSERT_EQ (2, pubs.size ());
	ASSERT_EQ (2, prvs.size ());
	ASSERT_NE (pubs.end (), pubs.find (key1.pub));
	ASSERT_NE (prvs.end (), prvs.find (key1.prv.as_private_key ()));
	ASSERT_NE (pubs.end (), pubs.find (key2.pub));
	ASSERT_NE (prvs.end (), prvs.find (key2.prv.as_private_key ()));
}

TEST (wallet, insufficient_spend_one)
{
	ysu::system system (1);
	ysu::keypair key1;
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	auto block (system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key1.pub, 500));
	ASSERT_NE (nullptr, block);
	ASSERT_EQ (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key1.pub, ysu::genesis_amount));
}

TEST (wallet, spend_all_one)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::block_hash latest1 (node1.latest (ysu::dev_genesis_key.pub));
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key2;
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, std::numeric_limits<ysu::uint128_t>::max ()));
	ysu::account_info info2;
	{
		auto transaction (node1.store.tx_begin_read ());
		node1.store.account_get (transaction, ysu::dev_genesis_key.pub, info2);
		ASSERT_NE (latest1, info2.head);
		auto block (node1.store.block_get (transaction, info2.head));
		ASSERT_NE (nullptr, block);
		ASSERT_EQ (latest1, block->previous ());
	}
	ASSERT_TRUE (info2.balance.is_zero ());
	ASSERT_EQ (0, node1.balance (ysu::dev_genesis_key.pub));
}

TEST (wallet, send_async)
{
	ysu::system system (1);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key2;
	std::thread thread ([&system]() {
		ASSERT_TIMELY (10s, system.nodes[0]->balance (ysu::dev_genesis_key.pub).is_zero ());
	});
	std::atomic<bool> success (false);
	system.wallet (0)->send_async (ysu::dev_genesis_key.pub, key2.pub, std::numeric_limits<ysu::uint128_t>::max (), [&success](std::shared_ptr<ysu::block> block_a) { ASSERT_NE (nullptr, block_a); success = true; });
	thread.join ();
	ASSERT_TIMELY (2s, success);
}

TEST (wallet, spend)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::block_hash latest1 (node1.latest (ysu::dev_genesis_key.pub));
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key2;
	// Sending from empty accounts should always be an error.  Accounts need to be opened with an open block, not a send block.
	ASSERT_EQ (nullptr, system.wallet (0)->send_action (0, key2.pub, 0));
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, std::numeric_limits<ysu::uint128_t>::max ()));
	ysu::account_info info2;
	{
		auto transaction (node1.store.tx_begin_read ());
		node1.store.account_get (transaction, ysu::dev_genesis_key.pub, info2);
		ASSERT_NE (latest1, info2.head);
		auto block (node1.store.block_get (transaction, info2.head));
		ASSERT_NE (nullptr, block);
		ASSERT_EQ (latest1, block->previous ());
	}
	ASSERT_TRUE (info2.balance.is_zero ());
	ASSERT_EQ (0, node1.balance (ysu::dev_genesis_key.pub));
}

TEST (wallet, change)
{
	ysu::system system (1);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key2;
	auto block1 (system.nodes[0]->rep_block (ysu::dev_genesis_key.pub));
	ASSERT_FALSE (block1.is_zero ());
	ASSERT_NE (nullptr, system.wallet (0)->change_action (ysu::dev_genesis_key.pub, key2.pub));
	auto block2 (system.nodes[0]->rep_block (ysu::dev_genesis_key.pub));
	ASSERT_FALSE (block2.is_zero ());
	ASSERT_NE (block1, block2);
}

TEST (wallet, partial_spend)
{
	ysu::system system (1);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key2;
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, 500));
	ASSERT_EQ (std::numeric_limits<ysu::uint128_t>::max () - 500, system.nodes[0]->balance (ysu::dev_genesis_key.pub));
}

TEST (wallet, spend_no_previous)
{
	ysu::system system (1);
	{
		system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
		auto transaction (system.nodes[0]->store.tx_begin_read ());
		ysu::account_info info1;
		ASSERT_FALSE (system.nodes[0]->store.account_get (transaction, ysu::dev_genesis_key.pub, info1));
		for (auto i (0); i < 50; ++i)
		{
			ysu::keypair key;
			system.wallet (0)->insert_adhoc (key.prv);
		}
	}
	ysu::keypair key2;
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, 500));
	ASSERT_EQ (std::numeric_limits<ysu::uint128_t>::max () - 500, system.nodes[0]->balance (ysu::dev_genesis_key.pub));
}

TEST (wallet, find_none)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (init);
	ysu::account account (1000);
	ASSERT_EQ (wallet.end (), wallet.find (transaction, account));
}

TEST (wallet, find_existing)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (init);
	ysu::keypair key1;
	ASSERT_FALSE (wallet.exists (transaction, key1.pub));
	wallet.insert_adhoc (transaction, key1.prv);
	ASSERT_TRUE (wallet.exists (transaction, key1.pub));
	auto existing (wallet.find (transaction, key1.pub));
	ASSERT_NE (wallet.end (), existing);
	++existing;
	ASSERT_EQ (wallet.end (), existing);
}

TEST (wallet, rekey)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (init);
	ysu::raw_key password;
	wallet.password.value (password);
	ASSERT_TRUE (password.data.is_zero ());
	ASSERT_FALSE (init);
	ysu::keypair key1;
	wallet.insert_adhoc (transaction, key1.prv);
	ysu::raw_key prv1;
	wallet.fetch (transaction, key1.pub, prv1);
	ASSERT_EQ (key1.prv, prv1);
	ASSERT_FALSE (wallet.rekey (transaction, "1"));
	wallet.password.value (password);
	ysu::raw_key password1;
	wallet.derive_key (password1, transaction, "1");
	ASSERT_EQ (password1, password);
	ysu::raw_key prv2;
	wallet.fetch (transaction, key1.pub, prv2);
	ASSERT_EQ (key1.prv, prv2);
	*wallet.password.values[0] = 2;
	ASSERT_TRUE (wallet.rekey (transaction, "2"));
}

TEST (account, encode_zero)
{
	ysu::account number0 (0);
	std::string str0;
	number0.encode_account (str0);

	/*
	 * Handle different lengths for "xrb_" prefixed and "ysu_" prefixed accounts
	 */
	ASSERT_EQ ((str0.front () == 'x') ? 64 : 65, str0.size ());
	ASSERT_EQ (65, str0.size ());
	ysu::account number1;
	ASSERT_FALSE (number1.decode_account (str0));
	ASSERT_EQ (number0, number1);
}

TEST (account, encode_all)
{
	ysu::account number0;
	number0.decode_hex ("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
	std::string str0;
	number0.encode_account (str0);

	/*
	 * Handle different lengths for "xrb_" prefixed and "ysu_" prefixed accounts
	 */
	ASSERT_EQ ((str0.front () == 'x') ? 64 : 65, str0.size ());
	ysu::account number1;
	ASSERT_FALSE (number1.decode_account (str0));
	ASSERT_EQ (number0, number1);
}

TEST (account, encode_fail)
{
	ysu::account number0 (0);
	std::string str0;
	number0.encode_account (str0);
	str0[16] ^= 1;
	ysu::account number1;
	ASSERT_TRUE (number1.decode_account (str0));
}

TEST (wallet, hash_password)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (init);
	ysu::raw_key hash1;
	wallet.derive_key (hash1, transaction, "");
	ysu::raw_key hash2;
	wallet.derive_key (hash2, transaction, "");
	ASSERT_EQ (hash1, hash2);
	ysu::raw_key hash3;
	wallet.derive_key (hash3, transaction, "a");
	ASSERT_NE (hash1, hash3);
}

TEST (fan, reconstitute)
{
	ysu::uint256_union value0 (0);
	ysu::fan fan (value0, 1024);
	for (auto & i : fan.values)
	{
		ASSERT_NE (value0, *i);
	}
	ysu::raw_key value1;
	fan.value (value1);
	ASSERT_EQ (value0, value1.data);
}

TEST (fan, change)
{
	ysu::raw_key value0;
	value0.data = 0;
	ysu::raw_key value1;
	value1.data = 1;
	ASSERT_NE (value0, value1);
	ysu::fan fan (value0.data, 1024);
	ASSERT_EQ (1024, fan.values.size ());
	ysu::raw_key value2;
	fan.value (value2);
	ASSERT_EQ (value0, value2);
	fan.value_set (value1);
	fan.value (value2);
	ASSERT_EQ (value1, value2);
}

TEST (wallet, reopen_default_password)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	auto transaction (env.tx_begin_write ());
	ASSERT_FALSE (init);
	ysu::kdf kdf;
	{
		ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
		ASSERT_FALSE (init);
		ASSERT_TRUE (wallet.valid_password (transaction));
	}
	{
		bool init;
		ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
		ASSERT_FALSE (init);
		ASSERT_TRUE (wallet.valid_password (transaction));
	}
	{
		ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
		ASSERT_FALSE (init);
		wallet.rekey (transaction, "");
		ASSERT_TRUE (wallet.valid_password (transaction));
	}
	{
		bool init;
		ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
		ASSERT_FALSE (init);
		ASSERT_FALSE (wallet.valid_password (transaction));
		wallet.attempt_password (transaction, " ");
		ASSERT_FALSE (wallet.valid_password (transaction));
		wallet.attempt_password (transaction, "");
		ASSERT_TRUE (wallet.valid_password (transaction));
	}
}

TEST (wallet, representative)
{
	auto error (false);
	ysu::mdb_env env (error, ysu::unique_path ());
	ASSERT_FALSE (error);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (error, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (error);
	ASSERT_FALSE (wallet.is_representative (transaction));
	ASSERT_EQ (ysu::genesis_account, wallet.representative (transaction));
	ASSERT_FALSE (wallet.is_representative (transaction));
	ysu::keypair key;
	wallet.representative_set (transaction, key.pub);
	ASSERT_FALSE (wallet.is_representative (transaction));
	ASSERT_EQ (key.pub, wallet.representative (transaction));
	ASSERT_FALSE (wallet.is_representative (transaction));
	wallet.insert_adhoc (transaction, key.prv);
	ASSERT_TRUE (wallet.is_representative (transaction));
}

TEST (wallet, serialize_json_empty)
{
	auto error (false);
	ysu::mdb_env env (error, ysu::unique_path ());
	ASSERT_FALSE (error);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet1 (error, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (error);
	std::string serialized;
	wallet1.serialize_json (transaction, serialized);
	ysu::wallet_store wallet2 (error, kdf, transaction, ysu::genesis_account, 1, "1", serialized);
	ASSERT_FALSE (error);
	ysu::raw_key password1;
	ysu::raw_key password2;
	wallet1.wallet_key (password1, transaction);
	wallet2.wallet_key (password2, transaction);
	ASSERT_EQ (password1, password2);
	ASSERT_EQ (wallet1.salt (transaction), wallet2.salt (transaction));
	ASSERT_EQ (wallet1.check (transaction), wallet2.check (transaction));
	ASSERT_EQ (wallet1.representative (transaction), wallet2.representative (transaction));
	ASSERT_EQ (wallet1.end (), wallet1.begin (transaction));
	ASSERT_EQ (wallet2.end (), wallet2.begin (transaction));
}

TEST (wallet, serialize_json_one)
{
	auto error (false);
	ysu::mdb_env env (error, ysu::unique_path ());
	ASSERT_FALSE (error);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet1 (error, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (error);
	ysu::keypair key;
	wallet1.insert_adhoc (transaction, key.prv);
	std::string serialized;
	wallet1.serialize_json (transaction, serialized);
	ysu::wallet_store wallet2 (error, kdf, transaction, ysu::genesis_account, 1, "1", serialized);
	ASSERT_FALSE (error);
	ysu::raw_key password1;
	ysu::raw_key password2;
	wallet1.wallet_key (password1, transaction);
	wallet2.wallet_key (password2, transaction);
	ASSERT_EQ (password1, password2);
	ASSERT_EQ (wallet1.salt (transaction), wallet2.salt (transaction));
	ASSERT_EQ (wallet1.check (transaction), wallet2.check (transaction));
	ASSERT_EQ (wallet1.representative (transaction), wallet2.representative (transaction));
	ASSERT_TRUE (wallet2.exists (transaction, key.pub));
	ysu::raw_key prv;
	wallet2.fetch (transaction, key.pub, prv);
	ASSERT_EQ (key.prv, prv);
}

TEST (wallet, serialize_json_password)
{
	auto error (false);
	ysu::mdb_env env (error, ysu::unique_path ());
	ASSERT_FALSE (error);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet1 (error, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (error);
	ysu::keypair key;
	wallet1.rekey (transaction, "password");
	wallet1.insert_adhoc (transaction, key.prv);
	std::string serialized;
	wallet1.serialize_json (transaction, serialized);
	ysu::wallet_store wallet2 (error, kdf, transaction, ysu::genesis_account, 1, "1", serialized);
	ASSERT_FALSE (error);
	ASSERT_FALSE (wallet2.valid_password (transaction));
	ASSERT_FALSE (wallet2.attempt_password (transaction, "password"));
	ASSERT_TRUE (wallet2.valid_password (transaction));
	ysu::raw_key password1;
	ysu::raw_key password2;
	wallet1.wallet_key (password1, transaction);
	wallet2.wallet_key (password2, transaction);
	ASSERT_EQ (password1, password2);
	ASSERT_EQ (wallet1.salt (transaction), wallet2.salt (transaction));
	ASSERT_EQ (wallet1.check (transaction), wallet2.check (transaction));
	ASSERT_EQ (wallet1.representative (transaction), wallet2.representative (transaction));
	ASSERT_TRUE (wallet2.exists (transaction, key.pub));
	ysu::raw_key prv;
	wallet2.fetch (transaction, key.pub, prv);
	ASSERT_EQ (key.prv, prv);
}

TEST (wallet_store, move)
{
	auto error (false);
	ysu::mdb_env env (error, ysu::unique_path ());
	ASSERT_FALSE (error);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet1 (error, kdf, transaction, ysu::genesis_account, 1, "0");
	ASSERT_FALSE (error);
	ysu::keypair key1;
	wallet1.insert_adhoc (transaction, key1.prv);
	ysu::wallet_store wallet2 (error, kdf, transaction, ysu::genesis_account, 1, "1");
	ASSERT_FALSE (error);
	ysu::keypair key2;
	wallet2.insert_adhoc (transaction, key2.prv);
	ASSERT_FALSE (wallet1.exists (transaction, key2.pub));
	ASSERT_TRUE (wallet2.exists (transaction, key2.pub));
	std::vector<ysu::public_key> keys;
	keys.push_back (key2.pub);
	ASSERT_FALSE (wallet1.move (transaction, wallet2, keys));
	ASSERT_TRUE (wallet1.exists (transaction, key2.pub));
	ASSERT_FALSE (wallet2.exists (transaction, key2.pub));
}

TEST (wallet_store, import)
{
	ysu::system system (2);
	auto wallet1 (system.wallet (0));
	auto wallet2 (system.wallet (1));
	ysu::keypair key1;
	wallet1->insert_adhoc (key1.prv);
	std::string json;
	wallet1->serialize (json);
	ASSERT_FALSE (wallet2->exists (key1.pub));
	auto error (wallet2->import (json, ""));
	ASSERT_FALSE (error);
	ASSERT_TRUE (wallet2->exists (key1.pub));
}

TEST (wallet_store, fail_import_bad_password)
{
	ysu::system system (2);
	auto wallet1 (system.wallet (0));
	auto wallet2 (system.wallet (1));
	ysu::keypair key1;
	wallet1->insert_adhoc (key1.prv);
	std::string json;
	wallet1->serialize (json);
	ASSERT_FALSE (wallet2->exists (key1.pub));
	auto error (wallet2->import (json, "1"));
	ASSERT_TRUE (error);
}

TEST (wallet_store, fail_import_corrupt)
{
	ysu::system system (2);
	auto wallet1 (system.wallet (1));
	std::string json;
	auto error (wallet1->import (json, "1"));
	ASSERT_TRUE (error);
}

// Test work is precached when a key is inserted
TEST (wallet, work)
{
	ysu::system system (1);
	auto wallet (system.wallet (0));
	wallet->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::genesis genesis;
	auto done (false);
	system.deadline_set (20s);
	while (!done)
	{
		auto transaction (system.wallet (0)->wallets.tx_begin_read ());
		uint64_t work (0);
		if (!wallet->store.work_get (transaction, ysu::dev_genesis_key.pub, work))
		{
			done = ysu::work_difficulty (genesis.open->work_version (), genesis.hash (), work) >= system.nodes[0]->default_difficulty (genesis.open->work_version ());
		}
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (wallet, work_generate)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	auto wallet (system.wallet (0));
	ysu::uint128_t amount1 (node1.balance (ysu::dev_genesis_key.pub));
	uint64_t work1;
	wallet->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::account account1;
	{
		auto transaction (node1.wallets.tx_begin_read ());
		account1 = system.account (transaction, 0);
	}
	ysu::keypair key;
	auto block (wallet->send_action (ysu::dev_genesis_key.pub, key.pub, 100));
	auto transaction (node1.store.tx_begin_read ());
	ASSERT_TIMELY (10s, node1.ledger.account_balance (transaction, ysu::dev_genesis_key.pub) != amount1);
	system.deadline_set (10s);
	auto again (true);
	while (again)
	{
		ASSERT_NO_ERROR (system.poll ());
		auto block_transaction (node1.store.tx_begin_read ());
		auto transaction (system.wallet (0)->wallets.tx_begin_read ());
		again = wallet->store.work_get (transaction, account1, work1) || ysu::work_difficulty (block->work_version (), node1.ledger.latest_root (block_transaction, account1), work1) < node1.default_difficulty (block->work_version ());
	}
}

TEST (wallet, work_cache_delayed)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	auto wallet (system.wallet (0));
	uint64_t work1;
	wallet->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::account account1;
	{
		auto transaction (node1.wallets.tx_begin_read ());
		account1 = system.account (transaction, 0);
	}
	ysu::keypair key;
	auto block1 (wallet->send_action (ysu::dev_genesis_key.pub, key.pub, 100));
	ASSERT_EQ (block1->hash (), node1.latest (ysu::dev_genesis_key.pub));
	auto block2 (wallet->send_action (ysu::dev_genesis_key.pub, key.pub, 100));
	ASSERT_EQ (block2->hash (), node1.latest (ysu::dev_genesis_key.pub));
	ASSERT_EQ (block2->hash (), node1.wallets.delayed_work->operator[] (ysu::dev_genesis_key.pub));
	auto threshold (node1.default_difficulty (ysu::work_version::work_1));
	auto again (true);
	system.deadline_set (10s);
	while (again)
	{
		ASSERT_NO_ERROR (system.poll ());
		if (!wallet->store.work_get (node1.wallets.tx_begin_read (), account1, work1))
		{
			again = ysu::work_difficulty (ysu::work_version::work_1, block2->hash (), work1) < threshold;
		}
	}
	ASSERT_GE (ysu::work_difficulty (ysu::work_version::work_1, block2->hash (), work1), threshold);
}

TEST (wallet, insert_locked)
{
	ysu::system system (1);
	auto wallet (system.wallet (0));
	{
		auto transaction (wallet->wallets.tx_begin_write ());
		wallet->store.rekey (transaction, "1");
		ASSERT_TRUE (wallet->store.valid_password (transaction));
		wallet->enter_password (transaction, "");
	}
	auto transaction (wallet->wallets.tx_begin_read ());
	ASSERT_FALSE (wallet->store.valid_password (transaction));
	ASSERT_TRUE (wallet->insert_adhoc (ysu::keypair ().prv).is_zero ());
}

TEST (wallet, deterministic_keys)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	auto key1 = wallet.deterministic_key (transaction, 0);
	auto key2 = wallet.deterministic_key (transaction, 0);
	ASSERT_EQ (key1, key2);
	auto key3 = wallet.deterministic_key (transaction, 1);
	ASSERT_NE (key1, key3);
	ASSERT_EQ (0, wallet.deterministic_index_get (transaction));
	wallet.deterministic_index_set (transaction, 1);
	ASSERT_EQ (1, wallet.deterministic_index_get (transaction));
	auto key4 (wallet.deterministic_insert (transaction));
	ysu::raw_key key5;
	ASSERT_FALSE (wallet.fetch (transaction, key4, key5));
	ASSERT_EQ (key3, key5.as_private_key ());
	ASSERT_EQ (2, wallet.deterministic_index_get (transaction));
	wallet.deterministic_index_set (transaction, 1);
	ASSERT_EQ (1, wallet.deterministic_index_get (transaction));
	wallet.erase (transaction, key4);
	ASSERT_FALSE (wallet.exists (transaction, key4));
	auto key8 (wallet.deterministic_insert (transaction));
	ASSERT_EQ (key4, key8);
	auto key6 (wallet.deterministic_insert (transaction));
	ysu::raw_key key7;
	ASSERT_FALSE (wallet.fetch (transaction, key6, key7));
	ASSERT_NE (key5, key7);
	ASSERT_EQ (3, wallet.deterministic_index_get (transaction));
	ysu::keypair key9;
	ASSERT_EQ (key9.pub, wallet.insert_adhoc (transaction, key9.prv));
	ASSERT_TRUE (wallet.exists (transaction, key9.pub));
	wallet.deterministic_clear (transaction);
	ASSERT_EQ (0, wallet.deterministic_index_get (transaction));
	ASSERT_FALSE (wallet.exists (transaction, key4));
	ASSERT_FALSE (wallet.exists (transaction, key6));
	ASSERT_FALSE (wallet.exists (transaction, key8));
	ASSERT_TRUE (wallet.exists (transaction, key9.pub));
}

TEST (wallet, reseed)
{
	bool init;
	ysu::mdb_env env (init, ysu::unique_path ());
	ASSERT_FALSE (init);
	auto transaction (env.tx_begin_write ());
	ysu::kdf kdf;
	ysu::wallet_store wallet (init, kdf, transaction, ysu::genesis_account, 1, "0");
	ysu::raw_key seed1;
	seed1.data = 1;
	ysu::raw_key seed2;
	seed2.data = 2;
	wallet.seed_set (transaction, seed1);
	ysu::raw_key seed3;
	wallet.seed (seed3, transaction);
	ASSERT_EQ (seed1, seed3);
	auto key1 (wallet.deterministic_insert (transaction));
	ASSERT_EQ (1, wallet.deterministic_index_get (transaction));
	wallet.seed_set (transaction, seed2);
	ASSERT_EQ (0, wallet.deterministic_index_get (transaction));
	ysu::raw_key seed4;
	wallet.seed (seed4, transaction);
	ASSERT_EQ (seed2, seed4);
	auto key2 (wallet.deterministic_insert (transaction));
	ASSERT_NE (key1, key2);
	wallet.seed_set (transaction, seed1);
	ysu::raw_key seed5;
	wallet.seed (seed5, transaction);
	ASSERT_EQ (seed1, seed5);
	auto key3 (wallet.deterministic_insert (transaction));
	ASSERT_EQ (key1, key3);
}

TEST (wallet, insert_deterministic_locked)
{
	ysu::system system (1);
	auto wallet (system.wallet (0));
	auto transaction (wallet->wallets.tx_begin_write ());
	wallet->store.rekey (transaction, "1");
	ASSERT_TRUE (wallet->store.valid_password (transaction));
	wallet->enter_password (transaction, "");
	ASSERT_FALSE (wallet->store.valid_password (transaction));
	ASSERT_TRUE (wallet->deterministic_insert (transaction).is_zero ());
}

TEST (wallet, no_work)
{
	ysu::system system (1);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv, false);
	ysu::keypair key2;
	auto block (system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, std::numeric_limits<ysu::uint128_t>::max (), false));
	ASSERT_NE (nullptr, block);
	ASSERT_NE (0, block->block_work ());
	ASSERT_GE (block->difficulty (), ysu::work_threshold (block->work_version (), block->sideband ().details));
	auto transaction (system.wallet (0)->wallets.tx_begin_read ());
	uint64_t cached_work (0);
	system.wallet (0)->store.work_get (transaction, ysu::dev_genesis_key.pub, cached_work);
	ASSERT_EQ (0, cached_work);
}

TEST (wallet, send_race)
{
	ysu::system system (1);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key2;
	for (auto i (1); i < 60; ++i)
	{
		ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, ysu::Gxrb_ratio));
		ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio * i, system.nodes[0]->balance (ysu::dev_genesis_key.pub));
	}
}

TEST (wallet, password_race)
{
	ysu::system system (1);
	ysu::thread_runner runner (system.io_ctx, system.nodes[0]->config.io_threads);
	auto wallet = system.wallet (0);
	std::thread thread ([&wallet]() {
		for (int i = 0; i < 100; i++)
		{
			auto transaction (wallet->wallets.tx_begin_write ());
			wallet->store.rekey (transaction, std::to_string (i));
		}
	});
	for (int i = 0; i < 100; i++)
	{
		auto transaction (wallet->wallets.tx_begin_read ());
		// Password should always be valid, the rekey operation should be atomic.
		bool ok = wallet->store.valid_password (transaction);
		EXPECT_TRUE (ok);
		if (!ok)
		{
			break;
		}
	}
	thread.join ();
	system.stop ();
	runner.join ();
}

TEST (wallet, password_race_corrupt_seed)
{
	ysu::system system (1);
	ysu::thread_runner runner (system.io_ctx, system.nodes[0]->config.io_threads);
	auto wallet = system.wallet (0);
	ysu::raw_key seed;
	{
		auto transaction (wallet->wallets.tx_begin_write ());
		ASSERT_FALSE (wallet->store.rekey (transaction, "4567"));
		wallet->store.seed (seed, transaction);
		ASSERT_FALSE (wallet->store.attempt_password (transaction, "4567"));
	}
	std::vector<std::thread> threads;
	for (int i = 0; i < 100; i++)
	{
		threads.emplace_back ([&wallet]() {
			for (int i = 0; i < 10; i++)
			{
				auto transaction (wallet->wallets.tx_begin_write ());
				wallet->store.rekey (transaction, "0000");
			}
		});
		threads.emplace_back ([&wallet]() {
			for (int i = 0; i < 10; i++)
			{
				auto transaction (wallet->wallets.tx_begin_write ());
				wallet->store.rekey (transaction, "1234");
			}
		});
		threads.emplace_back ([&wallet]() {
			for (int i = 0; i < 10; i++)
			{
				auto transaction (wallet->wallets.tx_begin_read ());
				wallet->store.attempt_password (transaction, "1234");
			}
		});
	}
	for (auto & thread : threads)
	{
		thread.join ();
	}
	system.stop ();
	runner.join ();
	{
		auto transaction (wallet->wallets.tx_begin_write ());
		if (!wallet->store.attempt_password (transaction, "1234"))
		{
			ysu::raw_key seed_now;
			wallet->store.seed (seed_now, transaction);
			ASSERT_TRUE (seed_now == seed);
		}
		else if (!wallet->store.attempt_password (transaction, "0000"))
		{
			ysu::raw_key seed_now;
			wallet->store.seed (seed_now, transaction);
			ASSERT_TRUE (seed_now == seed);
		}
		else if (!wallet->store.attempt_password (transaction, "4567"))
		{
			ysu::raw_key seed_now;
			wallet->store.seed (seed_now, transaction);
			ASSERT_TRUE (seed_now == seed);
		}
		else
		{
			ASSERT_FALSE (true);
		}
	}
}

TEST (wallet, change_seed)
{
	ysu::system system (1);
	auto wallet (system.wallet (0));
	wallet->enter_initial_password ();
	ysu::raw_key seed1;
	seed1.data = 1;
	ysu::public_key pub;
	uint32_t index (4);
	auto prv = ysu::deterministic_key (seed1, index);
	pub = ysu::pub_key (prv);
	wallet->insert_adhoc (ysu::dev_genesis_key.prv, false);
	auto block (wallet->send_action (ysu::dev_genesis_key.pub, pub, 100));
	ASSERT_NE (nullptr, block);
	system.nodes[0]->block_processor.flush ();
	{
		auto transaction (wallet->wallets.tx_begin_write ());
		wallet->change_seed (transaction, seed1);
		ysu::raw_key seed2;
		wallet->store.seed (seed2, transaction);
		ASSERT_EQ (seed1, seed2);
		ASSERT_EQ (index + 1, wallet->store.deterministic_index_get (transaction));
	}
	ASSERT_TRUE (wallet->exists (pub));
}

TEST (wallet, deterministic_restore)
{
	ysu::system system (1);
	auto wallet (system.wallet (0));
	wallet->enter_initial_password ();
	ysu::raw_key seed1;
	seed1.data = 1;
	ysu::public_key pub;
	uint32_t index (4);
	{
		auto transaction (wallet->wallets.tx_begin_write ());
		wallet->change_seed (transaction, seed1);
		ysu::raw_key seed2;
		wallet->store.seed (seed2, transaction);
		ASSERT_EQ (seed1, seed2);
		ASSERT_EQ (1, wallet->store.deterministic_index_get (transaction));
		auto prv = ysu::deterministic_key (seed1, index);
		pub = ysu::pub_key (prv);
	}
	wallet->insert_adhoc (ysu::dev_genesis_key.prv, false);
	auto block (wallet->send_action (ysu::dev_genesis_key.pub, pub, 100));
	ASSERT_NE (nullptr, block);
	system.nodes[0]->block_processor.flush ();
	{
		auto transaction (wallet->wallets.tx_begin_write ());
		wallet->deterministic_restore (transaction);
		ASSERT_EQ (index + 1, wallet->store.deterministic_index_get (transaction));
	}
	ASSERT_TRUE (wallet->exists (pub));
}

TEST (work_watcher, update)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.enable_voting = false;
	node_config.work_watcher_period = 1s;
	node_config.max_work_generate_multiplier = 1e6;
	ysu::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node = *system.add_node (node_config, node_flags);
	auto & wallet (*system.wallet (0));
	wallet.insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key;
	auto const block1 (wallet.send_action (ysu::dev_genesis_key.pub, key.pub, 100));
	auto difficulty1 (block1->difficulty ());
	auto multiplier1 (ysu::normalized_multiplier (ysu::difficulty::to_multiplier (difficulty1, ysu::work_threshold (block1->work_version (), ysu::block_details (ysu::epoch::epoch_0, true, false, false))), node.network_params.network.publish_thresholds.epoch_1));
	auto const block2 (wallet.send_action (ysu::dev_genesis_key.pub, key.pub, 200));
	auto difficulty2 (block2->difficulty ());
	auto multiplier2 (ysu::normalized_multiplier (ysu::difficulty::to_multiplier (difficulty2, ysu::work_threshold (block2->work_version (), ysu::block_details (ysu::epoch::epoch_0, true, false, false))), node.network_params.network.publish_thresholds.epoch_1));
	double updated_multiplier1{ multiplier1 }, updated_multiplier2{ multiplier2 }, target_multiplier{ std::max (multiplier1, multiplier2) + 1e-6 };
	{
		ysu::lock_guard<std::mutex> guard (node.active.mutex);
		node.active.trended_active_multiplier = target_multiplier;
	}
	system.deadline_set (20s);
	while (updated_multiplier1 == multiplier1 || updated_multiplier2 == multiplier2)
	{
		{
			ysu::lock_guard<std::mutex> guard (node.active.mutex);
			{
				auto const existing (node.active.roots.find (block1->qualified_root ()));
				//if existing is junk the block has been confirmed already
				ASSERT_NE (existing, node.active.roots.end ());
				updated_multiplier1 = existing->multiplier;
			}
			{
				auto const existing (node.active.roots.find (block2->qualified_root ()));
				//if existing is junk the block has been confirmed already
				ASSERT_NE (existing, node.active.roots.end ());
				updated_multiplier2 = existing->multiplier;
			}
		}
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_GT (updated_multiplier1, multiplier1);
	ASSERT_GT (updated_multiplier2, multiplier2);
}

TEST (work_watcher, propagate)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.enable_voting = false;
	node_config.work_watcher_period = 1s;
	node_config.max_work_generate_multiplier = 1e6;
	ysu::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node = *system.add_node (node_config, node_flags);
	auto & wallet (*system.wallet (0));
	wallet.insert_adhoc (ysu::dev_genesis_key.prv);
	node_config.peering_port = ysu::get_available_port ();
	auto & node_passive = *system.add_node (node_config);
	ysu::keypair key;
	auto const block (wallet.send_action (ysu::dev_genesis_key.pub, key.pub, 100));
	ASSERT_TIMELY (5s, node_passive.ledger.block_exists (block->hash ()));
	auto const multiplier (ysu::normalized_multiplier (ysu::difficulty::to_multiplier (block->difficulty (), ysu::work_threshold (block->work_version (), ysu::block_details (ysu::epoch::epoch_0, false, false, false))), node.network_params.network.publish_thresholds.epoch_1));
	auto updated_multiplier{ multiplier };
	auto propagated_multiplier{ multiplier };
	{
		ysu::lock_guard<std::mutex> guard (node.active.mutex);
		node.active.trended_active_multiplier = multiplier * 1.001;
	}
	bool updated{ false };
	bool propagated{ false };
	system.deadline_set (10s);
	while (!(updated && propagated))
	{
		{
			ysu::lock_guard<std::mutex> guard (node.active.mutex);
			{
				auto const existing (node.active.roots.find (block->qualified_root ()));
				ASSERT_NE (existing, node.active.roots.end ());
				updated_multiplier = existing->multiplier;
			}
		}
		{
			ysu::lock_guard<std::mutex> guard (node_passive.active.mutex);
			{
				auto const existing (node_passive.active.roots.find (block->qualified_root ()));
				ASSERT_NE (existing, node_passive.active.roots.end ());
				propagated_multiplier = existing->multiplier;
			}
		}
		updated = updated_multiplier != multiplier;
		propagated = propagated_multiplier != multiplier;
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_GT (updated_multiplier, multiplier);
	ASSERT_EQ (propagated_multiplier, updated_multiplier);
}

TEST (work_watcher, removed_after_win)
{
	ysu::system system (1);
	auto & node (*system.nodes[0]);
	auto & wallet (*system.wallet (0));
	wallet.insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key;
	ASSERT_EQ (0, wallet.wallets.watcher->size ());
	auto const block1 (wallet.send_action (ysu::dev_genesis_key.pub, key.pub, 100));
	ASSERT_EQ (1, wallet.wallets.watcher->size ());
	ASSERT_TIMELY (5s, !node.wallets.watcher->is_watched (block1->qualified_root ()));
	ASSERT_EQ (0, node.wallets.watcher->size ());
}

TEST (work_watcher, removed_after_lose)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.enable_voting = false;
	node_config.work_watcher_period = 1s;
	auto & node = *system.add_node (node_config);
	auto & wallet (*system.wallet (0));
	wallet.insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key;
	auto const block1 (wallet.send_action (ysu::dev_genesis_key.pub, key.pub, 100));
	ASSERT_TRUE (node.wallets.watcher->is_watched (block1->qualified_root ()));
	ysu::genesis genesis;
	auto fork1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::xrb_ratio, ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));
	node.process_active (fork1);
	node.block_processor.flush ();
	auto vote (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 0, fork1));
	ysu::confirm_ack message (vote);
	node.network.process_message (message, std::make_shared<ysu::transport::channel_loopback> (node));
	ASSERT_TIMELY (5s, !node.wallets.watcher->is_watched (block1->qualified_root ()));
	ASSERT_EQ (0, node.wallets.watcher->size ());
}

TEST (work_watcher, generation_disabled)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.enable_voting = false;
	node_config.work_watcher_period = 1s;
	node_config.work_threads = 0;
	ysu::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node = *system.add_node (node_config);
	ASSERT_FALSE (node.work_generation_enabled ());
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::genesis genesis;
	ysu::keypair key;
	auto block (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Mxrb_ratio, key.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ())));
	auto difficulty (block->difficulty ());
	node.wallets.watcher->add (block);
	ASSERT_FALSE (node.process_local (block).code != ysu::process_result::progress);
	ASSERT_TRUE (node.wallets.watcher->is_watched (block->qualified_root ()));
	auto multiplier = ysu::normalized_multiplier (ysu::difficulty::to_multiplier (difficulty, ysu::work_threshold (block->work_version (), ysu::block_details (ysu::epoch::epoch_0, true, false, false))), node.network_params.network.publish_thresholds.epoch_1);
	double updated_multiplier{ multiplier };
	{
		ysu::lock_guard<std::mutex> guard (node.active.mutex);
		node.active.trended_active_multiplier = multiplier * 10;
	}
	std::this_thread::sleep_for (2s);
	ASSERT_TRUE (node.wallets.watcher->is_watched (block->qualified_root ()));
	{
		ysu::lock_guard<std::mutex> guard (node.active.mutex);
		auto const existing (node.active.roots.find (block->qualified_root ()));
		ASSERT_NE (existing, node.active.roots.end ());
		updated_multiplier = existing->multiplier;
	}
	ASSERT_EQ (updated_multiplier, multiplier);
	ASSERT_EQ (0, node.distributed_work.size ());
}

TEST (work_watcher, cancel)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.work_watcher_period = 1s;
	node_config.max_work_generate_multiplier = 1e6;
	node_config.enable_voting = false;
	auto & node = *system.add_node (node_config);
	auto & wallet (*system.wallet (0));
	wallet.insert_adhoc (ysu::dev_genesis_key.prv, false);
	ysu::keypair key;
	auto work1 (node.work_generate_blocking (ysu::dev_genesis_key.pub));
	auto const block1 (wallet.send_action (ysu::dev_genesis_key.pub, key.pub, 100, *work1, false));
	{
		ysu::unique_lock<std::mutex> lock (node.active.mutex);
		// Prevent active difficulty repopulating multipliers
		node.network_params.network.request_interval_ms = 10000;
		// Fill multipliers_cb and update active difficulty;
		for (auto i (0); i < node.active.multipliers_cb.size (); i++)
		{
			node.active.multipliers_cb.push_back (node.config.max_work_generate_multiplier);
		}
		node.active.update_active_multiplier (lock);
	}
	// Wait for work generation to start
	ASSERT_TIMELY (5s, 0 != node.work.size ());
	// Cancel the ongoing work
	ASSERT_EQ (1, node.work.size ());
	node.work.cancel (block1->root ());
	ASSERT_EQ (0, node.work.size ());
	{
		ysu::unique_lock<std::mutex> lock (wallet.wallets.watcher->mutex);
		auto existing (wallet.wallets.watcher->watched.find (block1->qualified_root ()));
		ASSERT_NE (wallet.wallets.watcher->watched.end (), existing);
		auto block2 (existing->second);
		// Block must be the same
		ASSERT_NE (nullptr, block1);
		ASSERT_NE (nullptr, block2);
		ASSERT_EQ (*block1, *block2);
		// but should still be under watch
		lock.unlock ();
		ASSERT_TRUE (wallet.wallets.watcher->is_watched (block1->qualified_root ()));
	}
}

TEST (work_watcher, confirm_while_generating)
{
	// Ensure proper behavior when confirmation happens during work generation
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.work_threads = 1;
	node_config.work_watcher_period = 1s;
	node_config.max_work_generate_multiplier = 1e6;
	node_config.enable_voting = false;
	auto & node = *system.add_node (node_config);
	auto & wallet (*system.wallet (0));
	wallet.insert_adhoc (ysu::dev_genesis_key.prv, false);
	ysu::keypair key;
	auto work1 (node.work_generate_blocking (ysu::dev_genesis_key.pub));
	auto const block1 (wallet.send_action (ysu::dev_genesis_key.pub, key.pub, 100, *work1, false));
	{
		ysu::unique_lock<std::mutex> lock (node.active.mutex);
		// Prevent active difficulty repopulating multipliers
		node.network_params.network.request_interval_ms = 10000;
		// Fill multipliers_cb and update active difficulty;
		for (auto i (0); i < node.active.multipliers_cb.size (); i++)
		{
			node.active.multipliers_cb.push_back (node.config.max_work_generate_multiplier);
		}
		node.active.update_active_multiplier (lock);
	}
	// Wait for work generation to start
	ASSERT_TIMELY (5s, 0 != node.work.size ());
	// Attach a callback to work cancellations
	std::atomic<bool> notified{ false };
	node.observers.work_cancel.add ([&notified, &block1](ysu::root const & root_a) {
		EXPECT_EQ (root_a, block1->root ());
		notified = true;
	});
	// Confirm the block
	ASSERT_EQ (1, node.active.size ());
	auto election (node.active.election (block1->qualified_root ()));
	ASSERT_NE (nullptr, election);
	election->force_confirm ();
	ASSERT_TIMELY (5s, node.block_confirmed (block1->hash ()));
	ASSERT_EQ (0, node.work.size ());
	ASSERT_TRUE (notified);
	ASSERT_FALSE (node.wallets.watcher->is_watched (block1->qualified_root ()));
}

// Ensure the minimum limited difficulty is enough for the highest threshold
TEST (wallet, limited_difficulty)
{
	ysu::system system;
	ysu::genesis genesis;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.max_work_generate_multiplier = 1;
	ysu::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node = *system.add_node (node_config, node_flags);
	auto & wallet (*system.wallet (0));
	// Upgrade the genesis account to epoch 2
	ASSERT_NE (nullptr, system.upgrade_genesis_epoch (node, ysu::epoch::epoch_1));
	ASSERT_NE (nullptr, system.upgrade_genesis_epoch (node, ysu::epoch::epoch_2));
	ASSERT_EQ (ysu::epoch::epoch_2, node.store.block_version (node.store.tx_begin_read (), node.latest (ysu::dev_genesis_key.pub)));
	wallet.insert_adhoc (ysu::dev_genesis_key.prv, false);
	{
		// Force active difficulty to an impossibly high value
		ysu::lock_guard<std::mutex> guard (node.active.mutex);
		node.active.trended_active_multiplier = 1024 * 1024 * 1024;
	}
	ASSERT_EQ (node.max_work_generate_difficulty (ysu::work_version::work_1), node.active.limited_active_difficulty (*genesis.open));
	auto send = wallet.send_action (ysu::dev_genesis_key.pub, ysu::keypair ().pub, 1, 1);
	ASSERT_NE (nullptr, send);
	ASSERT_EQ (ysu::epoch::epoch_2, send->sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, send->sideband ().source_epoch); // Not used for send state blocks
}

TEST (wallet, epoch_2_validation)
{
	ysu::system system (1);
	auto & node (*system.nodes[0]);
	auto & wallet (*system.wallet (0));

	// Upgrade the genesis account to epoch 2
	ASSERT_NE (nullptr, system.upgrade_genesis_epoch (node, ysu::epoch::epoch_1));
	ASSERT_NE (nullptr, system.upgrade_genesis_epoch (node, ysu::epoch::epoch_2));

	wallet.insert_adhoc (ysu::dev_genesis_key.prv, false);

	// Test send and receive blocks
	// An epoch 2 receive block should be generated with lower difficulty with high probability
	auto tries = 0;
	auto max_tries = 20;
	auto amount = node.config.receive_minimum.number ();
	while (++tries < max_tries)
	{
		auto send = wallet.send_action (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.pub, amount, 1);
		ASSERT_NE (nullptr, send);
		ASSERT_EQ (ysu::epoch::epoch_2, send->sideband ().details.epoch);
		ASSERT_EQ (ysu::epoch::epoch_0, send->sideband ().source_epoch); // Not used for send state blocks

		auto receive = wallet.receive_action (*send, ysu::dev_genesis_key.pub, amount, 1);
		ASSERT_NE (nullptr, receive);
		if (receive->difficulty () < node.network_params.network.publish_thresholds.base)
		{
			ASSERT_GE (receive->difficulty (), node.network_params.network.publish_thresholds.epoch_2_receive);
			ASSERT_EQ (ysu::epoch::epoch_2, receive->sideband ().details.epoch);
			ASSERT_EQ (ysu::epoch::epoch_2, receive->sideband ().source_epoch);
			break;
		}
	}
	ASSERT_LT (tries, max_tries);

	// Test a change block
	ASSERT_NE (nullptr, wallet.change_action (ysu::dev_genesis_key.pub, ysu::keypair ().pub, 1));
}

// Receiving from an upgraded account uses the lower threshold and upgrades the receiving account
TEST (wallet, epoch_2_receive_propagation)
{
	auto tries = 0;
	auto const max_tries = 20;
	while (++tries < max_tries)
	{
		ysu::system system;
		ysu::node_flags node_flags;
		node_flags.disable_request_loop = true;
		auto & node (*system.add_node (node_flags));
		auto & wallet (*system.wallet (0));

		// Upgrade the genesis account to epoch 1
		auto epoch1 = system.upgrade_genesis_epoch (node, ysu::epoch::epoch_1);
		ASSERT_NE (nullptr, epoch1);

		ysu::keypair key;
		ysu::state_block_builder builder;

		// Send and open the account
		wallet.insert_adhoc (ysu::dev_genesis_key.prv, false);
		wallet.insert_adhoc (key.prv, false);
		auto amount = node.config.receive_minimum.number ();
		auto send1 = wallet.send_action (ysu::dev_genesis_key.pub, key.pub, amount, 1);
		ASSERT_NE (nullptr, send1);
		ASSERT_NE (nullptr, wallet.receive_action (*send1, ysu::dev_genesis_key.pub, amount, 1));

		// Upgrade the genesis account to epoch 2
		auto epoch2 = system.upgrade_genesis_epoch (node, ysu::epoch::epoch_2);
		ASSERT_NE (nullptr, epoch2);

		// Send a block
		auto send2 = wallet.send_action (ysu::dev_genesis_key.pub, key.pub, amount, 1);
		ASSERT_NE (nullptr, send2);

		// Receiving should use the lower difficulty
		{
			ysu::lock_guard<std::mutex> guard (node.active.mutex);
			node.active.trended_active_multiplier = 1.0;
		}
		auto receive2 = wallet.receive_action (*send2, key.pub, amount, 1);
		ASSERT_NE (nullptr, receive2);
		if (receive2->difficulty () < node.network_params.network.publish_thresholds.base)
		{
			ASSERT_GE (receive2->difficulty (), node.network_params.network.publish_thresholds.epoch_2_receive);
			ASSERT_EQ (ysu::epoch::epoch_2, node.store.block_version (node.store.tx_begin_read (), receive2->hash ()));
			ASSERT_EQ (ysu::epoch::epoch_2, receive2->sideband ().source_epoch);
			break;
		}
	}
	ASSERT_LT (tries, max_tries);
}

// Opening an upgraded account uses the lower threshold
TEST (wallet, epoch_2_receive_unopened)
{
	// Ensure the lower receive work is used when receiving
	auto tries = 0;
	auto const max_tries = 20;
	while (++tries < max_tries)
	{
		ysu::system system;
		ysu::node_flags node_flags;
		node_flags.disable_request_loop = true;
		auto & node (*system.add_node (node_flags));
		auto & wallet (*system.wallet (0));

		// Upgrade the genesis account to epoch 1
		auto epoch1 = system.upgrade_genesis_epoch (node, ysu::epoch::epoch_1);
		ASSERT_NE (nullptr, epoch1);

		ysu::keypair key;
		ysu::state_block_builder builder;

		// Send
		wallet.insert_adhoc (ysu::dev_genesis_key.prv, false);
		auto amount = node.config.receive_minimum.number ();
		auto send1 = wallet.send_action (ysu::dev_genesis_key.pub, key.pub, amount, 1);

		// Upgrade unopened account to epoch_2
		auto epoch2_unopened = ysu::state_block (key.pub, 0, 0, 0, node.network_params.ledger.epochs.link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (key.pub, node.network_params.network.publish_thresholds.epoch_2));
		ASSERT_EQ (ysu::process_result::progress, node.process (epoch2_unopened).code);

		wallet.insert_adhoc (key.prv, false);

		// Receiving should use the lower difficulty
		{
			ysu::lock_guard<std::mutex> guard (node.active.mutex);
			node.active.trended_active_multiplier = 1.0;
		}
		auto receive1 = wallet.receive_action (*send1, key.pub, amount, 1);
		ASSERT_NE (nullptr, receive1);
		if (receive1->difficulty () < node.network_params.network.publish_thresholds.base)
		{
			ASSERT_GE (receive1->difficulty (), node.network_params.network.publish_thresholds.epoch_2_receive);
			ASSERT_EQ (ysu::epoch::epoch_2, node.store.block_version (node.store.tx_begin_read (), receive1->hash ()));
			ASSERT_EQ (ysu::epoch::epoch_1, receive1->sideband ().source_epoch);
			break;
		}
	}
	ASSERT_LT (tries, max_tries);
}

TEST (wallet, foreach_representative_deadlock)
{
	ysu::system system (1);
	auto & node (*system.nodes[0]);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	node.wallets.compute_reps ();
	ASSERT_EQ (1, node.wallets.reps ().voting);
	node.wallets.foreach_representative ([&node](ysu::public_key const & pub, ysu::raw_key const & prv) {
		if (node.wallets.mutex.try_lock ())
		{
			node.wallets.mutex.unlock ();
		}
		else
		{
			ASSERT_FALSE (true);
		}
	});
}

TEST (wallet, search_pending)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.enable_voting = false;
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags flags;
	flags.disable_search_pending = true;
	auto & node (*system.add_node (config, flags));
	auto & wallet (*system.wallet (0));

	wallet.insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::block_builder builder;
	auto send = builder.state ()
	            .account (ysu::genesis_account)
	            .previous (ysu::genesis_hash)
	            .representative (ysu::genesis_account)
	            .balance (ysu::genesis_amount - node.config.receive_minimum.number ())
	            .link (ysu::genesis_account)
	            .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
	            .work (*system.work.generate (ysu::genesis_hash))
	            .build ();
	ASSERT_EQ (ysu::process_result::progress, node.process (*send).code);

	// Pending search should start an election
	ASSERT_TRUE (node.active.empty ());
	ASSERT_FALSE (wallet.search_pending ());
	auto election = node.active.election (send->qualified_root ());
	ASSERT_NE (nullptr, election);

	// Erase the key so the confirmation does not trigger an automatic receive
	wallet.store.erase (node.wallets.tx_begin_write (), ysu::genesis_account);

	// Now confirm the election
	election->force_confirm ();

	ASSERT_TIMELY (5s, node.block_confirmed (send->hash ()) && node.active.empty ());

	// Re-insert the key
	wallet.insert_adhoc (ysu::dev_genesis_key.prv);

	// Pending search should create the receive block
	ASSERT_EQ (2, node.ledger.cache.block_count);
	ASSERT_FALSE (wallet.search_pending ());
	ASSERT_TIMELY (3s, node.balance (ysu::genesis_account) == ysu::genesis_amount);
	auto receive_hash = node.ledger.latest (node.store.tx_begin_read (), ysu::genesis_account);
	auto receive = node.block (receive_hash);
	ASSERT_NE (nullptr, receive);
	ASSERT_EQ (receive->sideband ().height, 3);
	ASSERT_EQ (send->hash (), receive->link ().as_block_hash ());
}
