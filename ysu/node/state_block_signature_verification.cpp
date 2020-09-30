#include <ysu/lib/logger_mt.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/lib/threading.hpp>
#include <ysu/lib/timer.hpp>
#include <ysu/node/nodeconfig.hpp>
#include <ysu/node/signatures.hpp>
#include <ysu/node/state_block_signature_verification.hpp>
#include <ysu/secure/common.hpp>

#include <boost/format.hpp>

ysu::state_block_signature_verification::state_block_signature_verification (ysu::signature_checker & signature_checker, ysu::epochs & epochs, ysu::node_config & node_config, ysu::logger_mt & logger, uint64_t state_block_signature_verification_size) :
signature_checker (signature_checker),
epochs (epochs),
node_config (node_config),
logger (logger),
thread ([this, state_block_signature_verification_size]() {
	ysu::thread_role::set (ysu::thread_role::name::state_block_signature_verification);
	this->run (state_block_signature_verification_size);
})
{
}

ysu::state_block_signature_verification::~state_block_signature_verification ()
{
	stop ();
}

void ysu::state_block_signature_verification::stop ()
{
	{
		ysu::lock_guard<std::mutex> guard (mutex);
		stopped = true;
	}

	if (thread.joinable ())
	{
		condition.notify_one ();
		thread.join ();
	}
}

void ysu::state_block_signature_verification::run (uint64_t state_block_signature_verification_size)
{
	ysu::unique_lock<std::mutex> lk (mutex);
	while (!stopped)
	{
		if (!state_blocks.empty ())
		{
			size_t const max_verification_batch (state_block_signature_verification_size != 0 ? state_block_signature_verification_size : ysu::signature_checker::batch_size * (node_config.signature_checker_threads + 1));
			active = true;
			while (!state_blocks.empty () && !stopped)
			{
				auto items = setup_items (max_verification_batch);
				lk.unlock ();
				verify_state_blocks (items);
				lk.lock ();
			}
			active = false;
			lk.unlock ();
			transition_inactive_callback ();
			lk.lock ();
		}
		else
		{
			condition.wait (lk);
		}
	}
}

bool ysu::state_block_signature_verification::is_active ()
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return active;
}

void ysu::state_block_signature_verification::add (ysu::unchecked_info const & info_a)
{
	{
		ysu::lock_guard<std::mutex> guard (mutex);
		state_blocks.push_back (info_a);
	}
	condition.notify_one ();
}

size_t ysu::state_block_signature_verification::size ()
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return state_blocks.size ();
}

std::deque<ysu::unchecked_info> ysu::state_block_signature_verification::setup_items (size_t max_count)
{
	std::deque<ysu::unchecked_info> items;
	if (state_blocks.size () <= max_count)
	{
		items.swap (state_blocks);
	}
	else
	{
		for (auto i (0); i < max_count; ++i)
		{
			items.push_back (state_blocks.front ());
			state_blocks.pop_front ();
		}
		debug_assert (!state_blocks.empty ());
	}
	return items;
}

void ysu::state_block_signature_verification::verify_state_blocks (std::deque<ysu::unchecked_info> & items)
{
	if (!items.empty ())
	{
		ysu::timer<> timer_l;
		timer_l.start ();
		auto size (items.size ());
		std::vector<ysu::block_hash> hashes;
		hashes.reserve (size);
		std::vector<unsigned char const *> messages;
		messages.reserve (size);
		std::vector<size_t> lengths;
		lengths.reserve (size);
		std::vector<ysu::account> accounts;
		accounts.reserve (size);
		std::vector<unsigned char const *> pub_keys;
		pub_keys.reserve (size);
		std::vector<ysu::signature> blocks_signatures;
		blocks_signatures.reserve (size);
		std::vector<unsigned char const *> signatures;
		signatures.reserve (size);
		std::vector<int> verifications;
		verifications.resize (size, 0);
		for (auto & item : items)
		{
			hashes.push_back (item.block->hash ());
			messages.push_back (hashes.back ().bytes.data ());
			lengths.push_back (sizeof (decltype (hashes)::value_type));
			ysu::account account (item.block->account ());
			if (!item.block->link ().is_zero () && epochs.is_epoch_link (item.block->link ()))
			{
				account = epochs.signer (epochs.epoch (item.block->link ()));
			}
			else if (!item.account.is_zero ())
			{
				account = item.account;
			}
			accounts.push_back (account);
			pub_keys.push_back (accounts.back ().bytes.data ());
			blocks_signatures.push_back (item.block->block_signature ());
			signatures.push_back (blocks_signatures.back ().bytes.data ());
		}
		ysu::signature_check_set check = { size, messages.data (), lengths.data (), pub_keys.data (), signatures.data (), verifications.data () };
		signature_checker.verify (check);
		if (node_config.logging.timing_logging () && timer_l.stop () > std::chrono::milliseconds (10))
		{
			logger.try_log (boost::str (boost::format ("Batch verified %1% state blocks in %2% %3%") % size % timer_l.value ().count () % timer_l.unit ()));
		}
		blocks_verified_callback (items, verifications, hashes, blocks_signatures);
	}
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (state_block_signature_verification & state_block_signature_verification, const std::string & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "state_blocks", state_block_signature_verification.size (), sizeof (ysu::unchecked_info) }));
	return composite;
}
