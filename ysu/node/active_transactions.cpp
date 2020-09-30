#include <ysu/lib/threading.hpp>
#include <ysu/node/active_transactions.hpp>
#include <ysu/node/confirmation_height_processor.hpp>
#include <ysu/node/confirmation_solicitor.hpp>
#include <ysu/node/election.hpp>
#include <ysu/node/node.hpp>
#include <ysu/node/repcrawler.hpp>
#include <ysu/secure/blockstore.hpp>

#include <boost/format.hpp>
#include <boost/variant/get.hpp>

#include <numeric>

using namespace std::chrono;

size_t constexpr ysu::active_transactions::max_active_elections_frontier_insertion;

constexpr std::chrono::minutes ysu::active_transactions::expired_optimistic_election_info_cutoff;

ysu::active_transactions::active_transactions (ysu::node & node_a, ysu::confirmation_height_processor & confirmation_height_processor_a) :
recently_dropped (node_a.stats),
confirmation_height_processor (confirmation_height_processor_a),
node (node_a),
multipliers_cb (20, 1.),
trended_active_multiplier (1.0),
generator (node_a.config, node_a.ledger, node_a.wallets, node_a.vote_processor, node_a.history, node_a.network, node_a.stats),
check_all_elections_period (node_a.network_params.network.is_dev_network () ? 10ms : 5s),
election_time_to_live (node_a.network_params.network.is_dev_network () ? 0s : 2s),
prioritized_cutoff (std::max<size_t> (1, node_a.config.active_elections_size / 10)),
thread ([this]() {
	ysu::thread_role::set (ysu::thread_role::name::request_loop);
	request_loop ();
})
{
	// Register a callback which will get called after a block is cemented
	confirmation_height_processor.add_cemented_observer ([this](std::shared_ptr<ysu::block> callback_block_a) {
		this->block_cemented_callback (callback_block_a);
	});

	// Register a callback which will get called if a block is already cemented
	confirmation_height_processor.add_block_already_cemented_observer ([this](ysu::block_hash const & hash_a) {
		this->block_already_cemented_callback (hash_a);
	});

	ysu::unique_lock<std::mutex> lock (mutex);
	condition.wait (lock, [& started = started] { return started; });
}

ysu::active_transactions::~active_transactions ()
{
	stop ();
}

bool ysu::active_transactions::insert_election_from_frontiers_confirmation (std::shared_ptr<ysu::block> const & block_a, ysu::account const & account_a, ysu::uint128_t previous_balance_a, ysu::election_behavior election_behavior_a)
{
	bool inserted{ false };
	ysu::lock_guard<std::mutex> guard (mutex);
	if (roots.get<tag_root> ().find (block_a->qualified_root ()) == roots.get<tag_root> ().end ())
	{
		std::function<void(std::shared_ptr<ysu::block> const &)> election_confirmation_cb;
		if (election_behavior_a == ysu::election_behavior::optimistic)
		{
			election_confirmation_cb = [this](std::shared_ptr<ysu::block> const & block_a) {
				--optimistic_elections_count;
			};
		}

		auto insert_result = insert_impl (block_a, previous_balance_a, election_behavior_a, election_confirmation_cb);
		inserted = insert_result.inserted;
		if (inserted)
		{
			insert_result.election->transition_active ();
			if (insert_result.election->optimistic ())
			{
				++optimistic_elections_count;
			}
		}
	}
	return inserted;
}

ysu::frontiers_confirmation_info ysu::active_transactions::get_frontiers_confirmation_info ()
{
	// Limit maximum count of elections to start
	auto rep_counts (node.wallets.reps ());
	bool representative (node.config.enable_voting && rep_counts.voting > 0);
	bool half_princpal_representative (representative && rep_counts.half_principal > 0);
	/* Check less frequently for regular nodes in auto mode */
	bool agressive_mode (half_princpal_representative || node.config.frontiers_confirmation == ysu::frontiers_confirmation_mode::always);
	auto is_dev_network = node.network_params.network.is_dev_network ();
	auto roots_size = size ();
	auto check_time_exceeded = std::chrono::steady_clock::now () >= next_frontier_check;
	auto max_elections = max_active_elections_frontier_insertion;
	auto low_active_elections = roots_size < max_elections;
	bool wallets_check_required = (!skip_wallets || !priority_wallet_cementable_frontiers.empty ()) && !agressive_mode;
	// Minimise dropping real-time transactions, set the number of frontiers added to a factor of the maximum number of possible active elections
	auto max_active = node.config.active_elections_size / 20;
	if (roots_size <= max_active && (check_time_exceeded || wallets_check_required || (!is_dev_network && low_active_elections && agressive_mode)))
	{
		// When the number of active elections is low increase max number of elections for setting confirmation height.
		if (max_active > roots_size + max_elections)
		{
			max_elections = max_active - roots_size;
		}
	}
	else
	{
		max_elections = 0;
	}

	return ysu::frontiers_confirmation_info{ max_elections, agressive_mode };
}

void ysu::active_transactions::set_next_frontier_check (bool agressive_mode_a)
{
	auto request_interval (std::chrono::milliseconds (node.network_params.network.request_interval_ms));
	auto rel_time_next_frontier_check = request_interval * (agressive_mode_a ? 20 : 60);
	// Decrease check time for dev network
	int dev_network_factor = node.network_params.network.is_dev_network () ? 1000 : 1;

	next_frontier_check = steady_clock::now () + (rel_time_next_frontier_check / dev_network_factor);
}

void ysu::active_transactions::confirm_prioritized_frontiers (ysu::transaction const & transaction_a, uint64_t max_elections_a, uint64_t & elections_count_a)
{
	ysu::unique_lock<std::mutex> lk (mutex);
	auto start_elections_for_prioritized_frontiers = [&transaction_a, &elections_count_a, max_elections_a, &lk, this](prioritize_num_uncemented & cementable_frontiers) {
		while (!cementable_frontiers.empty () && !this->stopped && elections_count_a < max_elections_a && optimistic_elections_count < max_optimistic ())
		{
			auto cementable_account_front_it = cementable_frontiers.get<tag_uncemented> ().begin ();
			auto cementable_account = *cementable_account_front_it;
			cementable_frontiers.get<tag_uncemented> ().erase (cementable_account_front_it);
			if (expired_optimistic_election_infos.get<tag_account> ().count (cementable_account.account) == 0)
			{
				lk.unlock ();
				ysu::account_info info;
				auto error = this->node.store.account_get (transaction_a, cementable_account.account, info);
				if (!error)
				{
					if (!this->confirmation_height_processor.is_processing_block (info.head))
					{
						ysu::confirmation_height_info confirmation_height_info;
						error = this->node.store.confirmation_height_get (transaction_a, cementable_account.account, confirmation_height_info);
						release_assert (!error);

						if (info.block_count > confirmation_height_info.height)
						{
							auto block (this->node.store.block_get (transaction_a, info.head));
							auto previous_balance (this->node.ledger.balance (transaction_a, block->previous ()));
							auto inserted_election = this->insert_election_from_frontiers_confirmation (block, cementable_account.account, previous_balance, ysu::election_behavior::optimistic);
							if (inserted_election)
							{
								++elections_count_a;
							}
						}
					}
				}
				lk.lock ();
			}
		}
	};

	start_elections_for_prioritized_frontiers (priority_wallet_cementable_frontiers);
	start_elections_for_prioritized_frontiers (priority_cementable_frontiers);
}

void ysu::active_transactions::block_cemented_callback (std::shared_ptr<ysu::block> const & block_a)
{
	auto transaction = node.store.tx_begin_read ();

	boost::optional<ysu::election_status_type> election_status_type;
	if (!confirmation_height_processor.is_processing_added_block (block_a->hash ()))
	{
		election_status_type = confirm_block (transaction, block_a);
	}
	else
	{
		// This block was explicitly added to the confirmation height_processor
		election_status_type = ysu::election_status_type::active_confirmed_quorum;
	}

	if (election_status_type.is_initialized ())
	{
		if (election_status_type == ysu::election_status_type::inactive_confirmation_height)
		{
			ysu::account account (0);
			ysu::uint128_t amount (0);
			bool is_state_send (false);
			ysu::account pending_account (0);
			node.process_confirmed_data (transaction, block_a, block_a->hash (), account, amount, is_state_send, pending_account);
			node.observers.blocks.notify (ysu::election_status{ block_a, 0, std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now ().time_since_epoch ()), std::chrono::duration_values<std::chrono::milliseconds>::zero (), 0, 1, 0, ysu::election_status_type::inactive_confirmation_height }, account, amount, is_state_send);
		}
		else
		{
			auto hash (block_a->hash ());
			ysu::unique_lock<std::mutex> election_winners_lk (election_winner_details_mutex);
			auto existing (election_winner_details.find (hash));
			if (existing != election_winner_details.end ())
			{
				auto election = existing->second;
				election_winner_details.erase (hash);
				election_winners_lk.unlock ();
				ysu::unique_lock<std::mutex> lk (mutex);
				if (election->confirmed () && election->status.winner->hash () == hash)
				{
					add_recently_cemented (election->status);
					lk.unlock ();
					node.receive_confirmed (node.wallets.tx_begin_read (), transaction, block_a, hash);
					ysu::account account (0);
					ysu::uint128_t amount (0);
					bool is_state_send (false);
					ysu::account pending_account (0);
					node.process_confirmed_data (transaction, block_a, hash, account, amount, is_state_send, pending_account);
					lk.lock ();
					election->status.type = *election_status_type;
					election->status.confirmation_request_count = election->confirmation_request_count;
					auto status (election->status);
					lk.unlock ();
					node.observers.blocks.notify (status, account, amount, is_state_send);
					if (amount > 0)
					{
						node.observers.account_balance.notify (account, false);
						if (!pending_account.is_zero ())
						{
							node.observers.account_balance.notify (pending_account, true);
						}
					}
				}
			}
		}

		auto const & account (!block_a->account ().is_zero () ? block_a->account () : block_a->sideband ().account);
		debug_assert (!account.is_zero ());

		// Next-block activations are done after cementing hardcoded bootstrap count to allow confirming very large chains without interference
		bool const cemented_bootstrap_count_reached{ node.ledger.cache.cemented_count >= node.ledger.bootstrap_weight_max_blocks };

		// Next-block activations are only done for blocks with previously active elections
		bool const was_active{ *election_status_type == ysu::election_status_type::active_confirmed_quorum || *election_status_type == ysu::election_status_type::active_confirmation_height };

		// Activations are only done if there is not a large amount of active elections, ensuring frontier confirmation takes place
		auto const low_active_elections = [this] { return this->size () < ysu::active_transactions::max_active_elections_frontier_insertion / 2; };

		if (cemented_bootstrap_count_reached && was_active && low_active_elections ())
		{
			// Start or vote for the next unconfirmed block
			activate (account);

			// Start or vote for the next unconfirmed block in the destination account
			auto const & destination (node.ledger.block_destination (transaction, *block_a));
			if (!destination.is_zero () && destination != account)
			{
				activate (destination);
			}
		}
	}
}

void ysu::active_transactions::add_election_winner_details (ysu::block_hash const & hash_a, std::shared_ptr<ysu::election> const & election_a)
{
	ysu::lock_guard<std::mutex> guard (election_winner_details_mutex);
	election_winner_details.emplace (hash_a, election_a);
}

void ysu::active_transactions::remove_election_winner_details (ysu::block_hash const & hash_a)
{
	ysu::lock_guard<std::mutex> guard (election_winner_details_mutex);
	election_winner_details.erase (hash_a);
}

void ysu::active_transactions::block_already_cemented_callback (ysu::block_hash const & hash_a)
{
	// Depending on timing there is a situation where the election_winner_details is not reset.
	// This can happen when a block wins an election, and the block is confirmed + observer
	// called before the block hash gets added to election_winner_details. If the block is confirmed
	// callbacks have already been done, so we can safely just remove it.
	remove_election_winner_details (hash_a);
}

void ysu::active_transactions::request_confirm (ysu::unique_lock<std::mutex> & lock_a)
{
	debug_assert (!mutex.try_lock ());

	// Only representatives ready to receive batched confirm_req
	ysu::confirmation_solicitor solicitor (node.network, node.network_params.network);
	solicitor.prepare (node.rep_crawler.principal_representatives (std::numeric_limits<size_t>::max ()));

	ysu::vote_generator_session generator_session (generator);
	auto & sorted_roots_l (roots.get<tag_difficulty> ());
	auto const election_ttl_cutoff_l (std::chrono::steady_clock::now () - election_time_to_live);
	bool const check_all_elections_l (std::chrono::steady_clock::now () - last_check_all_elections > check_all_elections_period);
	size_t const this_loop_target_l (check_all_elections_l ? sorted_roots_l.size () : prioritized_cutoff);
	size_t unconfirmed_count_l (0);
	ysu::timer<std::chrono::milliseconds> elapsed (ysu::timer_state::started);

	/*
	 * Loop through active elections in descending order of proof-of-work difficulty, requesting confirmation
	 *
	 * Only up to a certain amount of elections are queued for confirmation request and block rebroadcasting. The remaining elections can still be confirmed if votes arrive
	 * Elections extending the soft config.active_elections_size limit are flushed after a certain time-to-live cutoff
	 * Flushed elections are later re-activated via frontier confirmation
	 */
	for (auto i = sorted_roots_l.begin (), n = sorted_roots_l.end (); i != n && unconfirmed_count_l < this_loop_target_l;)
	{
		auto & election_l (i->election);
		bool const confirmed_l (election_l->confirmed ());

		if (!election_l->prioritized () && unconfirmed_count_l < prioritized_cutoff)
		{
			election_l->prioritize_election (generator_session);
		}

		unconfirmed_count_l += !confirmed_l;
		bool const overflow_l (unconfirmed_count_l > node.config.active_elections_size && election_l->election_start < election_ttl_cutoff_l && !node.wallets.watcher->is_watched (i->root));
		if (overflow_l || election_l->transition_time (solicitor))
		{
			if (election_l->optimistic () && election_l->failed ())
			{
				if (election_l->confirmation_request_count != 0)
				{
					add_expired_optimistic_election (*election_l);
				}
				--optimistic_elections_count;
			}

			cleanup_election (election_l->cleanup_info ());
			i = sorted_roots_l.erase (i);
		}
		else
		{
			++i;
		}
	}
	lock_a.unlock ();
	solicitor.flush ();
	generator_session.flush ();
	lock_a.lock ();

	// This is updated after the loop to ensure slow machines don't do the full check often
	if (check_all_elections_l)
	{
		last_check_all_elections = std::chrono::steady_clock::now ();
		if (node.config.logging.timing_logging () && this_loop_target_l > prioritized_cutoff)
		{
			node.logger.try_log (boost::str (boost::format ("Processed %1% elections (%2% were already confirmed) in %3% %4%") % this_loop_target_l % (this_loop_target_l - unconfirmed_count_l) % elapsed.value ().count () % elapsed.unit ()));
		}
	}
}

void ysu::active_transactions::cleanup_election (ysu::election_cleanup_info const & info_a)
{
	debug_assert (!mutex.try_lock ());

	for (auto const & [hash, block] : info_a.blocks)
	{
		auto erased (blocks.erase (hash));
		(void)erased;
		debug_assert (erased == 1);
		erase_inactive_votes_cache (hash);
		// Notify observers about dropped elections & blocks lost confirmed elections
		if (!info_a.confirmed || hash != info_a.winner)
		{
			node.observers.active_stopped.notify (hash);
		}
	}

	if (!info_a.confirmed)
	{
		recently_dropped.add (info_a.root);

		// Clear network filter in another thread
		node.worker.push_task ([node_l = node.shared (), blocks_l = std::move (info_a.blocks)]() {
			for (auto const & block : blocks_l)
			{
				node_l->network.publish_filter.clear (block.second);
			}
		});
	}
}

void ysu::active_transactions::add_expired_optimistic_election (ysu::election const & election_a)
{
	auto account = election_a.status.winner->account ();
	if (account.is_zero ())
	{
		account = election_a.status.winner->sideband ().account;
	}

	auto it = expired_optimistic_election_infos.get<tag_account> ().find (account);
	if (it != expired_optimistic_election_infos.get<tag_account> ().end ())
	{
		expired_optimistic_election_infos.get<tag_account> ().modify (it, [](auto & expired_optimistic_election) {
			expired_optimistic_election.expired_time = std::chrono::steady_clock::now ();
			expired_optimistic_election.election_started = false;
		});
	}
	else
	{
		expired_optimistic_election_infos.emplace (std::chrono::steady_clock::now (), account);
	}

	// Expire the oldest one if a maximum is reached
	auto const max_expired_optimistic_election_infos = 10000;
	if (expired_optimistic_election_infos.size () > max_expired_optimistic_election_infos)
	{
		expired_optimistic_election_infos.get<tag_expired_time> ().erase (expired_optimistic_election_infos.get<tag_expired_time> ().begin ());
	}
	expired_optimistic_election_infos_size = expired_optimistic_election_infos.size ();
}

unsigned ysu::active_transactions::max_optimistic ()
{
	return node.ledger.cache.cemented_count < node.ledger.bootstrap_weight_max_blocks ? std::numeric_limits<unsigned>::max () : 50u;
}

void ysu::active_transactions::frontiers_confirmation (ysu::unique_lock<std::mutex> & lock_a)
{
	// Spend some time prioritizing accounts with the most uncemented blocks to reduce voting traffic
	auto request_interval = std::chrono::milliseconds (node.network_params.network.request_interval_ms);
	// Spend longer searching ledger accounts when there is a low amount of elections going on
	auto low_active = roots.size () < 1000;
	auto time_to_spend_prioritizing_ledger_accounts = request_interval / (low_active ? 20 : 100);
	auto time_to_spend_prioritizing_wallet_accounts = request_interval / 250;
	auto time_to_spend_confirming_pessimistic_accounts = time_to_spend_prioritizing_ledger_accounts;
	lock_a.unlock ();
	auto transaction = node.store.tx_begin_read ();
	prioritize_frontiers_for_confirmation (transaction, node.network_params.network.is_dev_network () ? std::chrono::milliseconds (50) : time_to_spend_prioritizing_ledger_accounts, time_to_spend_prioritizing_wallet_accounts);
	auto frontiers_confirmation_info = get_frontiers_confirmation_info ();
	if (frontiers_confirmation_info.can_start_elections ())
	{
		uint64_t elections_count (0);
		confirm_prioritized_frontiers (transaction, frontiers_confirmation_info.max_elections, elections_count);
		confirm_expired_frontiers_pessimistically (transaction, frontiers_confirmation_info.max_elections, elections_count);
		set_next_frontier_check (frontiers_confirmation_info.aggressive_mode);
	}
	lock_a.lock ();
}

/*
 * This function takes the expired_optimistic_election_infos generated from failed elections from frontiers confirmations and starts
 * confirming blocks at cemented height + 1 (cemented frontier successor) for an account only if all dependent blocks already
 * confirmed.
 */
void ysu::active_transactions::confirm_expired_frontiers_pessimistically (ysu::transaction const & transaction_a, uint64_t max_elections_a, uint64_t & elections_count_a)
{
	auto i{ node.store.accounts_begin (transaction_a, next_frontier_account) };
	auto n{ node.store.accounts_end () };
	ysu::timer<std::chrono::milliseconds> timer (ysu::timer_state::started);
	ysu::confirmation_height_info confirmation_height_info;

	// Loop through any expired optimistic elections which have not been started yet. This tag stores already started ones first
	std::vector<ysu::account> elections_started_for_account;
	for (auto i = expired_optimistic_election_infos.get<tag_election_started> ().lower_bound (false); i != expired_optimistic_election_infos.get<tag_election_started> ().end ();)
	{
		if (stopped || elections_count_a >= max_elections_a)
		{
			break;
		}

		auto const & account{ i->account };
		ysu::account_info account_info;
		bool should_delete{ true };
		if (!node.store.account_get (transaction_a, account, account_info) && !node.store.confirmation_height_get (transaction_a, account, confirmation_height_info))
		{
			if (account_info.block_count > confirmation_height_info.height)
			{
				should_delete = false;
				std::shared_ptr<ysu::block> previous_block;
				std::shared_ptr<ysu::block> block;
				if (confirmation_height_info.height == 0)
				{
					block = node.store.block_get (transaction_a, account_info.open_block);
				}
				else
				{
					previous_block = node.store.block_get (transaction_a, confirmation_height_info.frontier);
					block = node.store.block_get (transaction_a, previous_block->sideband ().successor);
				}

				if (block && !node.confirmation_height_processor.is_processing_block (block->hash ()) && node.ledger.dependents_confirmed (transaction_a, *block))
				{
					ysu::uint128_t previous_balance{ 0 };
					if (previous_block && previous_block->balance ().is_zero ())
					{
						previous_balance = previous_block->sideband ().balance.number ();
					}

					auto inserted_election = insert_election_from_frontiers_confirmation (block, account, previous_balance, ysu::election_behavior::normal);
					if (inserted_election)
					{
						++elections_count_a;
					}
					elections_started_for_account.push_back (i->account);
				}
			}
		}

		if (should_delete)
		{
			// This account is confirmed already or doesn't exist.
			i = expired_optimistic_election_infos.get<tag_election_started> ().erase (i);
			expired_optimistic_election_infos_size = expired_optimistic_election_infos.size ();
		}
		else
		{
			++i;
		}
	}

	for (auto const & account : elections_started_for_account)
	{
		auto it = expired_optimistic_election_infos.get<tag_account> ().find (account);
		debug_assert (it != expired_optimistic_election_infos.get<tag_account> ().end ());
		expired_optimistic_election_infos.get<tag_account> ().modify (it, [](auto & expired_optimistic_election_info_a) {
			expired_optimistic_election_info_a.election_started = true;
		});
	}
}

bool ysu::active_transactions::should_do_frontiers_confirmation () const
{
	/*
 	 * Confirm frontiers when there aren't many confirmations already pending and node finished initial bootstrap
 	 */
	auto pending_confirmation_height_size (confirmation_height_processor.awaiting_processing_size ());
	auto bootstrap_weight_reached (node.ledger.cache.block_count >= node.ledger.bootstrap_weight_max_blocks);
	auto disabled_confirmation_mode = (node.config.frontiers_confirmation == ysu::frontiers_confirmation_mode::disabled);
	auto conf_height_capacity_reached = pending_confirmation_height_size > confirmed_frontiers_max_pending_size;
	auto all_cemented = node.ledger.cache.block_count == node.ledger.cache.cemented_count;
	return (!disabled_confirmation_mode && bootstrap_weight_reached && !conf_height_capacity_reached && !all_cemented);
}

void ysu::active_transactions::request_loop ()
{
	ysu::unique_lock<std::mutex> lock (mutex);
	started = true;
	lock.unlock ();
	condition.notify_all ();

	// The wallets and active_transactions objects are mutually dependent, so we need a fully
	// constructed node before proceeding.
	this->node.node_initialized_latch.wait ();

	lock.lock ();

	while (!stopped && !node.flags.disable_request_loop)
	{
		// If many votes are queued, ensure at least the currently active ones finish processing
		lock.unlock ();
		if (node.vote_processor.half_full ())
		{
			node.vote_processor.flush_active ();
		}
		lock.lock ();

		const auto stamp_l = std::chrono::steady_clock::now ();

		// frontiers_confirmation should be above update_active_multiplier to ensure new sorted roots are updated
		if (should_do_frontiers_confirmation ())
		{
			frontiers_confirmation (lock);
		}
		update_active_multiplier (lock);
		request_confirm (lock);

		if (!stopped)
		{
			const auto min_sleep_l = std::chrono::milliseconds (node.network_params.network.request_interval_ms / 2);
			const auto wakeup_l = std::max (stamp_l + std::chrono::milliseconds (node.network_params.network.request_interval_ms), std::chrono::steady_clock::now () + min_sleep_l);
			condition.wait_until (lock, wakeup_l, [&wakeup_l, &stopped = stopped] { return stopped || std::chrono::steady_clock::now () >= wakeup_l; });
		}
	}
}

bool ysu::active_transactions::prioritize_account_for_confirmation (ysu::active_transactions::prioritize_num_uncemented & cementable_frontiers_a, size_t & cementable_frontiers_size_a, ysu::account const & account_a, ysu::account_info const & info_a, uint64_t confirmation_height_a)
{
	auto inserted_new{ false };
	if (info_a.block_count > confirmation_height_a && !confirmation_height_processor.is_processing_block (info_a.head))
	{
		auto num_uncemented = info_a.block_count - confirmation_height_a;
		ysu::lock_guard<std::mutex> guard (mutex);
		auto it = cementable_frontiers_a.get<tag_account> ().find (account_a);
		if (it != cementable_frontiers_a.get<tag_account> ().end ())
		{
			if (it->blocks_uncemented != num_uncemented)
			{
				// Account already exists and there is now a different uncemented block count so update it in the container
				cementable_frontiers_a.get<tag_account> ().modify (it, [num_uncemented](ysu::cementable_account & info) {
					info.blocks_uncemented = num_uncemented;
				});
			}
		}
		else
		{
			debug_assert (cementable_frontiers_size_a <= max_priority_cementable_frontiers);
			if (cementable_frontiers_size_a == max_priority_cementable_frontiers)
			{
				// The maximum amount of frontiers stored has been reached. Check if the current frontier
				// has more uncemented blocks than the lowest uncemented frontier in the collection if so replace it.
				auto least_uncemented_frontier_it = cementable_frontiers_a.get<tag_uncemented> ().end ();
				--least_uncemented_frontier_it;
				if (num_uncemented > least_uncemented_frontier_it->blocks_uncemented)
				{
					cementable_frontiers_a.get<tag_uncemented> ().erase (least_uncemented_frontier_it);
					cementable_frontiers_a.get<tag_account> ().emplace (account_a, num_uncemented);
				}
			}
			else
			{
				inserted_new = true;
				cementable_frontiers_a.get<tag_account> ().emplace (account_a, num_uncemented);
			}
		}
		cementable_frontiers_size_a = cementable_frontiers_a.size ();
	}
	return inserted_new;
}

void ysu::active_transactions::prioritize_frontiers_for_confirmation (ysu::transaction const & transaction_a, std::chrono::milliseconds ledger_account_traversal_max_time_a, std::chrono::milliseconds wallet_account_traversal_max_time_a)
{
	// Don't try to prioritize when there are a large number of pending confirmation heights as blocks can be cemented in the meantime, making the prioritization less reliable
	if (confirmation_height_processor.awaiting_processing_size () < confirmed_frontiers_max_pending_size)
	{
		size_t priority_cementable_frontiers_size;
		size_t priority_wallet_cementable_frontiers_size;
		{
			ysu::lock_guard<std::mutex> guard (mutex);
			priority_cementable_frontiers_size = priority_cementable_frontiers.size ();
			priority_wallet_cementable_frontiers_size = priority_wallet_cementable_frontiers.size ();
		}

		ysu::timer<std::chrono::milliseconds> wallet_account_timer (ysu::timer_state::started);
		// Remove any old expired optimistic elections so they are no longer excluded in subsequent checks
		auto expired_cutoff_it (expired_optimistic_election_infos.get<tag_expired_time> ().lower_bound (std::chrono::steady_clock::now () - expired_optimistic_election_info_cutoff));
		expired_optimistic_election_infos.get<tag_expired_time> ().erase (expired_optimistic_election_infos.get<tag_expired_time> ().begin (), expired_cutoff_it);
		expired_optimistic_election_infos_size = expired_optimistic_election_infos.size ();

		auto num_new_inserted{ 0u };
		auto should_iterate = [this, &num_new_inserted]() {
			auto max_optimistic_l = max_optimistic ();
			return !stopped && (max_optimistic_l > optimistic_elections_count && max_optimistic_l - optimistic_elections_count > num_new_inserted);
		};

		if (!skip_wallets)
		{
			// Prioritize wallet accounts first
			{
				ysu::lock_guard<std::mutex> lock (node.wallets.mutex);
				auto wallet_transaction (node.wallets.tx_begin_read ());
				auto const & items = node.wallets.items;
				if (items.empty ())
				{
					skip_wallets = true;
				}
				for (auto item_it = items.cbegin (); item_it != items.cend () && should_iterate (); ++item_it)
				{
					// Skip this wallet if it has been traversed already while there are others still awaiting
					if (wallet_ids_already_iterated.find (item_it->first) != wallet_ids_already_iterated.end ())
					{
						continue;
					}

					ysu::account_info info;
					auto & wallet (item_it->second);
					ysu::lock_guard<std::recursive_mutex> wallet_lock (wallet->store.mutex);

					auto & next_wallet_frontier_account = next_wallet_id_accounts.emplace (item_it->first, wallet_store::special_count).first->second;

					auto i (wallet->store.begin (wallet_transaction, next_wallet_frontier_account));
					auto n (wallet->store.end ());
					ysu::confirmation_height_info confirmation_height_info;
					for (; i != n && should_iterate (); ++i)
					{
						auto const & account (i->first);
						if (expired_optimistic_election_infos.get<tag_account> ().count (account) == 0 && !node.store.account_get (transaction_a, account, info) && !node.store.confirmation_height_get (transaction_a, account, confirmation_height_info))
						{
							// If it exists in normal priority collection delete from there.
							auto it = priority_cementable_frontiers.find (account);
							if (it != priority_cementable_frontiers.end ())
							{
								ysu::lock_guard<std::mutex> guard (mutex);
								priority_cementable_frontiers.erase (it);
								priority_cementable_frontiers_size = priority_cementable_frontiers.size ();
							}

							auto insert_newed = prioritize_account_for_confirmation (priority_wallet_cementable_frontiers, priority_wallet_cementable_frontiers_size, account, info, confirmation_height_info.height);
							if (insert_newed)
							{
								++num_new_inserted;
							}

							if (wallet_account_timer.since_start () >= wallet_account_traversal_max_time_a)
							{
								break;
							}
						}
						next_wallet_frontier_account = account.number () + 1;
					}
					// Go back to the beginning when we have reached the end of the wallet accounts for this wallet
					if (i == n)
					{
						wallet_ids_already_iterated.emplace (item_it->first);
						next_wallet_id_accounts.at (item_it->first) = wallet_store::special_count;

						// Skip wallet accounts when they have all been traversed
						if (std::next (item_it) == items.cend ())
						{
							wallet_ids_already_iterated.clear ();
							skip_wallets = true;
						}
					}
				}
			}
		}

		ysu::timer<std::chrono::milliseconds> timer (ysu::timer_state::started);
		auto i (node.store.accounts_begin (transaction_a, next_frontier_account));
		auto n (node.store.accounts_end ());
		ysu::confirmation_height_info confirmation_height_info;
		for (; i != n && should_iterate (); ++i)
		{
			auto const & account (i->first);
			auto const & info (i->second);
			if (priority_wallet_cementable_frontiers.find (account) == priority_wallet_cementable_frontiers.end ())
			{
				if (expired_optimistic_election_infos.get<tag_account> ().count (account) == 0 && !node.store.confirmation_height_get (transaction_a, account, confirmation_height_info))
				{
					auto insert_newed = prioritize_account_for_confirmation (priority_cementable_frontiers, priority_cementable_frontiers_size, account, info, confirmation_height_info.height);
					if (insert_newed)
					{
						++num_new_inserted;
					}
				}
			}
			next_frontier_account = account.number () + 1;
			if (timer.since_start () >= ledger_account_traversal_max_time_a)
			{
				break;
			}
		}

		// Go back to the beginning when we have reached the end of the accounts and start with wallet accounts next time
		if (i == n)
		{
			next_frontier_account = 0;
			skip_wallets = false;
		}
	}
}

void ysu::active_transactions::stop ()
{
	ysu::unique_lock<std::mutex> lock (mutex);
	if (!started)
	{
		condition.wait (lock, [& started = started] { return started; });
	}
	stopped = true;
	lock.unlock ();
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
	generator.stop ();
	lock.lock ();
	roots.clear ();
}

ysu::election_insertion_result ysu::active_transactions::insert_impl (std::shared_ptr<ysu::block> const & block_a, boost::optional<ysu::uint128_t> const & previous_balance_a, ysu::election_behavior election_behavior_a, std::function<void(std::shared_ptr<ysu::block>)> const & confirmation_action_a)
{
	debug_assert (block_a->has_sideband ());
	ysu::election_insertion_result result;
	if (!stopped)
	{
		auto root (block_a->qualified_root ());
		auto existing (roots.get<tag_root> ().find (root));
		if (existing == roots.get<tag_root> ().end ())
		{
			if (recently_confirmed.get<tag_root> ().find (root) == recently_confirmed.get<tag_root> ().end ())
			{
				result.inserted = true;
				auto hash (block_a->hash ());
				auto epoch (block_a->sideband ().details.epoch);
				ysu::uint128_t previous_balance (previous_balance_a.value_or (0));
				debug_assert (!(previous_balance_a.value_or (0) > 0 && block_a->previous ().is_zero ()));
				if (!previous_balance_a.is_initialized () && !block_a->previous ().is_zero ())
				{
					auto transaction (node.store.tx_begin_read ());
					if (node.ledger.block_exists (block_a->previous ()))
					{
						previous_balance = node.ledger.balance (transaction, block_a->previous ());
					}
				}
				double multiplier (normalized_multiplier (*block_a));
				bool prioritized = roots.size () < prioritized_cutoff || multiplier > last_prioritized_multiplier.value_or (0);
				result.election = ysu::make_shared<ysu::election> (node, block_a, confirmation_action_a, prioritized, election_behavior_a);
				roots.get<tag_root> ().emplace (ysu::active_transactions::conflict_info{ root, multiplier, result.election, epoch, previous_balance });
				blocks.emplace (hash, result.election);
				result.election->insert_inactive_votes_cache (hash);
				node.stats.inc (ysu::stat::type::election, prioritized ? ysu::stat::detail::election_priority : ysu::stat::detail::election_non_priority);
			}
		}
		else
		{
			result.election = existing->election;
		}

		// Votes are generated for inserted or ongoing elections if they're prioritized
		// Non-priority elections generate votes when they gain priority in the future
		if (result.election && result.election->prioritized ())
		{
			result.election->generate_votes ();
		}
	}
	return result;
}

ysu::election_insertion_result ysu::active_transactions::insert (std::shared_ptr<ysu::block> const & block_a, boost::optional<ysu::uint128_t> const & previous_balance_a, ysu::election_behavior election_behavior_a, std::function<void(std::shared_ptr<ysu::block>)> const & confirmation_action_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	return insert_impl (block_a, previous_balance_a, election_behavior_a, confirmation_action_a);
}

// Validate a vote and apply it to the current election if one exists
ysu::vote_code ysu::active_transactions::vote (std::shared_ptr<ysu::vote> vote_a)
{
	// If none of the hashes are active, votes are not republished
	bool at_least_one (false);
	// If all hashes were recently confirmed then it is a replay
	unsigned recently_confirmed_counter (0);
	bool replay (false);
	bool processed (false);
	{
		ysu::lock_guard<std::mutex> lock (mutex);
		for (auto vote_block : vote_a->blocks)
		{
			ysu::election_vote_result result;
			auto & recently_confirmed_by_hash (recently_confirmed.get<tag_hash> ());
			if (vote_block.which ())
			{
				auto block_hash (boost::get<ysu::block_hash> (vote_block));
				auto existing (blocks.find (block_hash));
				if (existing != blocks.end ())
				{
					at_least_one = true;
					result = existing->second->vote (vote_a->account, vote_a->sequence, block_hash);
				}
				else if (recently_confirmed_by_hash.count (block_hash) == 0)
				{
					add_inactive_votes_cache (block_hash, vote_a->account);
				}
				else
				{
					++recently_confirmed_counter;
				}
			}
			else
			{
				auto block (boost::get<std::shared_ptr<ysu::block>> (vote_block));
				auto existing (roots.get<tag_root> ().find (block->qualified_root ()));
				if (existing != roots.get<tag_root> ().end ())
				{
					at_least_one = true;
					result = existing->election->vote (vote_a->account, vote_a->sequence, block->hash ());
				}
				else if (recently_confirmed_by_hash.count (block->hash ()) == 0)
				{
					add_inactive_votes_cache (block->hash (), vote_a->account);
				}
				else
				{
					++recently_confirmed_counter;
				}
			}
			processed = processed || result.processed;
			replay = replay || result.replay;
		}
	}

	if (at_least_one)
	{
		// Republish vote if it is new and the node does not host a principal representative (or close to)
		if (processed)
		{
			auto const reps (node.wallets.reps ());
			if (!reps.have_half_rep () && !reps.exists (vote_a->account))
			{
				node.network.flood_vote (vote_a, 0.5f);
			}
		}
		return replay ? ysu::vote_code::replay : ysu::vote_code::vote;
	}
	else if (recently_confirmed_counter == vote_a->blocks.size ())
	{
		return ysu::vote_code::replay;
	}
	else
	{
		return ysu::vote_code::indeterminate;
	}
}

bool ysu::active_transactions::active (ysu::qualified_root const & root_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	return roots.get<tag_root> ().find (root_a) != roots.get<tag_root> ().end ();
}

bool ysu::active_transactions::active (ysu::block const & block_a)
{
	return active (block_a.qualified_root ());
}

std::shared_ptr<ysu::election> ysu::active_transactions::election (ysu::qualified_root const & root_a) const
{
	std::shared_ptr<ysu::election> result;
	ysu::lock_guard<std::mutex> lock (mutex);
	auto existing = roots.get<tag_root> ().find (root_a);
	if (existing != roots.get<tag_root> ().end ())
	{
		result = existing->election;
	}
	return result;
}

std::shared_ptr<ysu::block> ysu::active_transactions::winner (ysu::block_hash const & hash_a) const
{
	std::shared_ptr<ysu::block> result;
	ysu::lock_guard<std::mutex> lock (mutex);
	auto existing = blocks.find (hash_a);
	if (existing != blocks.end ())
	{
		result = existing->second->status.winner;
	}
	return result;
}

ysu::election_insertion_result ysu::active_transactions::activate (ysu::account const & account_a)
{
	ysu::election_insertion_result result;
	auto transaction (node.store.tx_begin_read ());
	ysu::account_info account_info;
	if (!node.store.account_get (transaction, account_a, account_info))
	{
		ysu::confirmation_height_info conf_info;
		auto error = node.store.confirmation_height_get (transaction, account_a, conf_info);
		debug_assert (!error);
		if (!error && conf_info.height < account_info.block_count)
		{
			debug_assert (conf_info.frontier != account_info.head);
			auto hash = conf_info.height == 0 ? account_info.open_block : node.store.block_successor (transaction, conf_info.frontier);
			auto block = node.store.block_get (transaction, hash);
			release_assert (block != nullptr);
			if (node.ledger.dependents_confirmed (transaction, *block))
			{
				result = insert (block);
				if (result.inserted)
				{
					result.election->transition_active ();
				}
			}
		}
	}
	return result;
}

bool ysu::active_transactions::update_difficulty (ysu::block const & block_a)
{
	ysu::lock_guard<std::mutex> guard (mutex);
	auto existing_election (roots.get<tag_root> ().find (block_a.qualified_root ()));
	bool error = existing_election == roots.get<tag_root> ().end () || update_difficulty_impl (existing_election, block_a);
	return error;
}

bool ysu::active_transactions::update_difficulty_impl (ysu::active_transactions::roots_iterator const & root_it_a, ysu::block const & block_a)
{
	debug_assert (!mutex.try_lock ());
	double multiplier (normalized_multiplier (block_a, root_it_a));
	bool error = multiplier <= root_it_a->multiplier;
	if (!error)
	{
		if (node.config.logging.active_update_logging ())
		{
			node.logger.try_log (boost::str (boost::format ("Election %1% difficulty updated with block %2% from multiplier %3% to %4%") % root_it_a->root.to_string () % block_a.hash ().to_string () % root_it_a->multiplier % multiplier));
		}
		roots.get<tag_root> ().modify (root_it_a, [multiplier](ysu::active_transactions::conflict_info & info_a) {
			info_a.multiplier = multiplier;
		});
		node.stats.inc (ysu::stat::type::election, ysu::stat::detail::election_difficulty_update);
	}
	return error;
}

bool ysu::active_transactions::restart (std::shared_ptr<ysu::block> const & block_a, ysu::write_transaction const & transaction_a)
{
	// Only guaranteed to restart the election if the new block is received within 2 minutes of its election being dropped
	constexpr std::chrono::minutes recently_dropped_cutoff{ 2 };
	bool error = true;
	if (recently_dropped.find (block_a->qualified_root ()) > std::chrono::steady_clock::now () - recently_dropped_cutoff)
	{
		auto hash (block_a->hash ());
		auto ledger_block (node.store.block_get (transaction_a, hash));
		if (ledger_block != nullptr && ledger_block->block_work () != block_a->block_work () && !node.block_confirmed_or_being_confirmed (transaction_a, hash))
		{
			if (block_a->difficulty () > ledger_block->difficulty ())
			{
				// Re-writing the block is necessary to avoid the same work being received later to force restarting the election
				// The existing block is re-written, not the arriving block, as that one might not have gone through a full signature check
				ledger_block->block_work_set (block_a->block_work ());

				auto block_count = node.ledger.cache.block_count.load ();
				node.store.block_put (transaction_a, hash, *ledger_block);
				debug_assert (node.ledger.cache.block_count.load () == block_count);

				// Restart election for the upgraded block, previously dropped from elections
				auto previous_balance = node.ledger.balance (transaction_a, ledger_block->previous ());
				auto insert_result = insert (ledger_block, previous_balance);
				if (insert_result.inserted)
				{
					error = false;
					insert_result.election->transition_active ();
					recently_dropped.erase (ledger_block->qualified_root ());
					node.stats.inc (ysu::stat::type::election, ysu::stat::detail::election_restart);
				}
			}
		}
	}
	return error;
}

double ysu::active_transactions::normalized_multiplier (ysu::block const & block_a, boost::optional<ysu::active_transactions::roots_iterator> const & root_it_a) const
{
	debug_assert (!mutex.try_lock ());
	auto difficulty (block_a.difficulty ());
	uint64_t threshold (0);
	bool sideband_not_found (false);
	if (block_a.has_sideband ())
	{
		threshold = ysu::work_threshold (block_a.work_version (), block_a.sideband ().details);
	}
	else if (root_it_a.is_initialized ())
	{
		auto election (*root_it_a);
		debug_assert (election != roots.end ());
		auto find_block (election->election->last_blocks.find (block_a.hash ()));
		if (find_block != election->election->last_blocks.end () && find_block->second->has_sideband ())
		{
			threshold = ysu::work_threshold (block_a.work_version (), find_block->second->sideband ().details);
		}
		else
		{
			// This can have incorrect results during an epoch upgrade, but it only affects prioritization
			bool is_send = election->previous_balance > block_a.balance ().number ();
			bool is_receive = election->previous_balance < block_a.balance ().number ();
			ysu::block_details details (election->epoch, is_send, is_receive, false);

			threshold = ysu::work_threshold (block_a.work_version (), details);
			sideband_not_found = true;
		}
	}
	double multiplier (ysu::difficulty::to_multiplier (difficulty, threshold));
	debug_assert (multiplier >= 1 || sideband_not_found);
	if (multiplier >= 1)
	{
		multiplier = ysu::normalized_multiplier (multiplier, threshold);
	}
	else
	{
		// Inferred threshold was incorrect
		multiplier = 1;
	}
	return multiplier;
}

void ysu::active_transactions::update_active_multiplier (ysu::unique_lock<std::mutex> & lock_a)
{
	debug_assert (!mutex.try_lock ());
	last_prioritized_multiplier.reset ();
	double multiplier (1.);
	// Heurestic to filter out non-saturated network and frontier confirmation
	if (roots.size () >= prioritized_cutoff || (node.network_params.network.is_dev_network () && !roots.empty ()))
	{
		auto & sorted_roots = roots.get<tag_difficulty> ();
		std::vector<double> prioritized;
		prioritized.reserve (std::min (sorted_roots.size (), prioritized_cutoff));
		for (auto it (sorted_roots.begin ()), end (sorted_roots.end ()); it != end && prioritized.size () < prioritized_cutoff; ++it)
		{
			if (!it->election->confirmed ())
			{
				prioritized.push_back (it->multiplier);
			}
		}
		if (prioritized.size () > 10 || (node.network_params.network.is_dev_network () && !prioritized.empty ()))
		{
			multiplier = prioritized[prioritized.size () / 2];
		}
		if (!prioritized.empty ())
		{
			last_prioritized_multiplier = prioritized.back ();
		}
	}
	debug_assert (multiplier >= ysu::difficulty::to_multiplier (node.network_params.network.publish_thresholds.entry, node.network_params.network.publish_thresholds.base));
	multipliers_cb.push_front (multiplier);
	auto sum (std::accumulate (multipliers_cb.begin (), multipliers_cb.end (), double(0)));
	double avg_multiplier (sum / multipliers_cb.size ());
	auto difficulty = ysu::difficulty::from_multiplier (avg_multiplier, node.default_difficulty (ysu::work_version::work_1));
	debug_assert (difficulty >= node.network_params.network.publish_thresholds.entry);

	trended_active_multiplier = avg_multiplier;
	lock_a.unlock ();
	node.observers.difficulty.notify (difficulty);
	lock_a.lock ();
}

uint64_t ysu::active_transactions::active_difficulty ()
{
	return ysu::difficulty::from_multiplier (active_multiplier (), node.default_difficulty (ysu::work_version::work_1));
}

uint64_t ysu::active_transactions::limited_active_difficulty (ysu::block const & block_a)
{
	uint64_t threshold (0);
	if (block_a.has_sideband ())
	{
		threshold = ysu::work_threshold (block_a.work_version (), block_a.sideband ().details);
	}
	else
	{
		threshold = node.default_difficulty (block_a.work_version ());
	}
	return limited_active_difficulty (block_a.work_version (), threshold);
}

uint64_t ysu::active_transactions::limited_active_difficulty (ysu::work_version const version_a, uint64_t const threshold_a)
{
	auto difficulty (ysu::difficulty::from_multiplier (ysu::denormalized_multiplier (active_multiplier (), threshold_a), threshold_a));
	return std::min (difficulty, node.max_work_generate_difficulty (version_a));
}

double ysu::active_transactions::active_multiplier ()
{
	return trended_active_multiplier.load ();
}

std::deque<ysu::election_status> ysu::active_transactions::list_recently_cemented ()
{
	ysu::lock_guard<std::mutex> lock (mutex);
	return recently_cemented;
}

void ysu::active_transactions::add_recently_cemented (ysu::election_status const & status_a)
{
	recently_cemented.push_back (status_a);
	if (recently_cemented.size () > node.config.confirmation_history_size)
	{
		recently_cemented.pop_front ();
	}
}

void ysu::active_transactions::add_recently_confirmed (ysu::qualified_root const & root_a, ysu::block_hash const & hash_a)
{
	recently_confirmed.get<tag_sequence> ().emplace_back (root_a, hash_a);
	if (recently_confirmed.size () > recently_confirmed_size)
	{
		recently_confirmed.get<tag_sequence> ().pop_front ();
	}
}

void ysu::active_transactions::erase_recently_confirmed (ysu::block_hash const & hash_a)
{
	ysu::lock_guard<std::mutex> guard (mutex);
	recently_confirmed.get<tag_hash> ().erase (hash_a);
}

void ysu::active_transactions::erase (ysu::block const & block_a)
{
	ysu::unique_lock<std::mutex> lock (mutex);
	auto root_it (roots.get<tag_root> ().find (block_a.qualified_root ()));
	if (root_it != roots.get<tag_root> ().end ())
	{
		cleanup_election (root_it->election->cleanup_info ());
		roots.get<tag_root> ().erase (root_it);
		lock.unlock ();
		node.logger.try_log (boost::str (boost::format ("Election erased for block block %1% root %2%") % block_a.hash ().to_string () % block_a.root ().to_string ()));
	}
}

bool ysu::active_transactions::empty ()
{
	ysu::lock_guard<std::mutex> lock (mutex);
	return roots.empty ();
}

size_t ysu::active_transactions::size ()
{
	ysu::lock_guard<std::mutex> lock (mutex);
	return roots.size ();
}

bool ysu::active_transactions::publish (std::shared_ptr<ysu::block> block_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	auto existing (roots.get<tag_root> ().find (block_a->qualified_root ()));
	auto result (true);
	if (existing != roots.get<tag_root> ().end ())
	{
		update_difficulty_impl (existing, *block_a);
		auto election (existing->election);
		result = election->publish (block_a);
		if (!result)
		{
			blocks.emplace (block_a->hash (), election);
			node.stats.inc (ysu::stat::type::election, ysu::stat::detail::election_block_conflict);
		}
	}
	return result;
}

// Returns the type of election status requiring callbacks calling later
boost::optional<ysu::election_status_type> ysu::active_transactions::confirm_block (ysu::transaction const & transaction_a, std::shared_ptr<ysu::block> block_a)
{
	auto hash (block_a->hash ());
	ysu::unique_lock<std::mutex> lock (mutex);
	auto existing (blocks.find (hash));
	boost::optional<ysu::election_status_type> status_type;
	if (existing != blocks.end ())
	{
		if (existing->second->status.winner && existing->second->status.winner->hash () == hash)
		{
			if (!existing->second->confirmed ())
			{
				existing->second->confirm_once (ysu::election_status_type::active_confirmation_height);
				status_type = ysu::election_status_type::active_confirmation_height;
			}
			else
			{
#ifndef NDEBUG
				ysu::unique_lock<std::mutex> election_winners_lk (election_winner_details_mutex);
				debug_assert (election_winner_details.find (hash) != election_winner_details.cend ());
#endif
				status_type = ysu::election_status_type::active_confirmed_quorum;
			}
		}
		else
		{
			status_type = boost::optional<ysu::election_status_type>{};
		}
	}
	else
	{
		status_type = ysu::election_status_type::inactive_confirmation_height;
	}

	return status_type;
}

size_t ysu::active_transactions::priority_cementable_frontiers_size ()
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return priority_cementable_frontiers.size ();
}

size_t ysu::active_transactions::priority_wallet_cementable_frontiers_size ()
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return priority_wallet_cementable_frontiers.size ();
}

boost::circular_buffer<double> ysu::active_transactions::difficulty_trend ()
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return multipliers_cb;
}

size_t ysu::active_transactions::inactive_votes_cache_size ()
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return inactive_votes_cache.size ();
}

void ysu::active_transactions::add_inactive_votes_cache (ysu::block_hash const & hash_a, ysu::account const & representative_a)
{
	// Check principal representative status
	if (node.ledger.weight (representative_a) > node.minimum_principal_weight ())
	{
		auto & inactive_by_hash (inactive_votes_cache.get<tag_hash> ());
		auto existing (inactive_by_hash.find (hash_a));
		if (existing != inactive_by_hash.end ())
		{
			if (existing->needs_eval ())
			{
				auto is_new (false);
				inactive_by_hash.modify (existing, [representative_a, &is_new](ysu::inactive_cache_information & info) {
					auto it = std::find (info.voters.begin (), info.voters.end (), representative_a);
					is_new = (it == info.voters.end ());
					if (is_new)
					{
						info.arrival = std::chrono::steady_clock::now ();
						info.voters.push_back (representative_a);
					}
				});

				if (is_new)
				{
					auto const status = inactive_votes_bootstrap_check (existing->voters, hash_a, existing->status);
					if (status != existing->status)
					{
						// The iterator is only valid if the container was unchanged, e.g., by erasing this item after inserting an election
						debug_assert (inactive_by_hash.count (hash_a));
						inactive_by_hash.modify (existing, [status](ysu::inactive_cache_information & info) {
							info.status = status;
						});
					}
				}
			}
		}
		else
		{
			std::vector<ysu::account> representative_vector{ representative_a };
			auto const status (inactive_votes_bootstrap_check (representative_vector, hash_a, {}));
			auto & inactive_by_arrival (inactive_votes_cache.get<tag_arrival> ());
			inactive_by_arrival.emplace (ysu::inactive_cache_information{ std::chrono::steady_clock::now (), hash_a, representative_vector, status });
			if (inactive_votes_cache.size () > node.flags.inactive_votes_cache_size)
			{
				inactive_by_arrival.erase (inactive_by_arrival.begin ());
			}
		}
	}
}

void ysu::active_transactions::trigger_inactive_votes_cache_election (std::shared_ptr<ysu::block> const & block_a)
{
	ysu::lock_guard<std::mutex> guard (mutex);
	auto const status = find_inactive_votes_cache (block_a->hash ()).status;
	if (status.election_started)
	{
		insert_impl (block_a);
	}
}

ysu::inactive_cache_information ysu::active_transactions::find_inactive_votes_cache (ysu::block_hash const & hash_a)
{
	auto & inactive_by_hash (inactive_votes_cache.get<tag_hash> ());
	auto existing (inactive_by_hash.find (hash_a));
	if (existing != inactive_by_hash.end ())
	{
		return *existing;
	}
	else
	{
		return ysu::inactive_cache_information{};
	}
}

void ysu::active_transactions::erase_inactive_votes_cache (ysu::block_hash const & hash_a)
{
	inactive_votes_cache.get<tag_hash> ().erase (hash_a);
}

ysu::inactive_cache_status ysu::active_transactions::inactive_votes_bootstrap_check (std::vector<ysu::account> const & voters_a, ysu::block_hash const & hash_a, ysu::inactive_cache_status const & previously_a)
{
	/** Perform checks on accumulated tally from inactive votes
	 * These votes are generally either for unconfirmed blocks or old confirmed blocks
	 * That check is made after hitting a tally threshold, and always as late and as few times as possible
	 */
	ysu::inactive_cache_status status (previously_a);
	constexpr unsigned election_start_voters_min{ 5 };
	ysu::uint128_t tally;
	for (auto const & voter : voters_a)
	{
		tally += node.ledger.weight (voter);
	}

	if (!previously_a.confirmed && tally >= node.config.online_weight_minimum.number ())
	{
		status.bootstrap_started = true;
		status.confirmed = true;
	}
	else if (!previously_a.bootstrap_started && !node.flags.disable_legacy_bootstrap && node.flags.disable_lazy_bootstrap && tally > node.gap_cache.bootstrap_threshold ())
	{
		status.bootstrap_started = true;
	}
	if (!previously_a.election_started && voters_a.size () >= election_start_voters_min && tally >= (node.online_reps.online_stake () / 100) * node.config.election_hint_weight_percent)
	{
		status.election_started = true;
	}

	if ((status.election_started && !previously_a.election_started) || (status.bootstrap_started && !previously_a.bootstrap_started))
	{
		auto transaction (node.store.tx_begin_read ());
		auto block = node.store.block_get (transaction, hash_a);
		if (block && status.election_started && !previously_a.election_started && !node.block_confirmed_or_being_confirmed (transaction, hash_a))
		{
			if (node.ledger.cache.cemented_count >= node.ledger.bootstrap_weight_max_blocks)
			{
				insert_impl (block);
			}
		}
		else if (!block && status.bootstrap_started && !previously_a.bootstrap_started)
		{
			node.gap_cache.bootstrap_start (hash_a);
		}
	}
	return status;
}

size_t ysu::active_transactions::election_winner_details_size ()
{
	ysu::lock_guard<std::mutex> guard (election_winner_details_mutex);
	return election_winner_details.size ();
}

ysu::cementable_account::cementable_account (ysu::account const & account_a, size_t blocks_uncemented_a) :
account (account_a), blocks_uncemented (blocks_uncemented_a)
{
}

ysu::expired_optimistic_election_info::expired_optimistic_election_info (std::chrono::steady_clock::time_point expired_time_a, ysu::account account_a) :
expired_time (expired_time_a),
account (account_a)
{
}

bool ysu::frontiers_confirmation_info::can_start_elections () const
{
	return max_elections > 0;
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (active_transactions & active_transactions, const std::string & name)
{
	size_t roots_count;
	size_t blocks_count;
	size_t recently_confirmed_count;
	size_t recently_cemented_count;

	{
		ysu::lock_guard<std::mutex> guard (active_transactions.mutex);
		roots_count = active_transactions.roots.size ();
		blocks_count = active_transactions.blocks.size ();
		recently_confirmed_count = active_transactions.recently_confirmed.size ();
		recently_cemented_count = active_transactions.recently_cemented.size ();
	}

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "roots", roots_count, sizeof (decltype (active_transactions.roots)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocks", blocks_count, sizeof (decltype (active_transactions.blocks)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "election_winner_details", active_transactions.election_winner_details_size (), sizeof (decltype (active_transactions.election_winner_details)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "recently_confirmed", recently_confirmed_count, sizeof (decltype (active_transactions.recently_confirmed)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "recently_cemented", recently_cemented_count, sizeof (decltype (active_transactions.recently_cemented)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "priority_wallet_cementable_frontiers", active_transactions.priority_wallet_cementable_frontiers_size (), sizeof (ysu::cementable_account) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "priority_cementable_frontiers", active_transactions.priority_cementable_frontiers_size (), sizeof (ysu::cementable_account) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "expired_optimistic_election_infos", active_transactions.expired_optimistic_election_infos_size, sizeof (decltype (active_transactions.expired_optimistic_election_infos)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "inactive_votes_cache", active_transactions.inactive_votes_cache_size (), sizeof (ysu::gap_information) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "optimistic_elections_count", active_transactions.optimistic_elections_count, 0 })); // This isn't an extra container, is just to expose the count easily
	composite->add_component (collect_container_info (active_transactions.generator, "generator"));
	return composite;
}

ysu::dropped_elections::dropped_elections (ysu::stat & stats_a) :
stats (stats_a)
{
}

void ysu::dropped_elections::add (ysu::qualified_root const & root_a)
{
	stats.inc (ysu::stat::type::election, ysu::stat::detail::election_drop);
	ysu::lock_guard<std::mutex> guard (mutex);
	auto & items_by_sequence = items.get<tag_sequence> ();
	items_by_sequence.emplace_back (ysu::election_timepoint{ std::chrono::steady_clock::now (), root_a });
	if (items.size () > capacity)
	{
		items_by_sequence.pop_front ();
	}
}

void ysu::dropped_elections::erase (ysu::qualified_root const & root_a)
{
	ysu::lock_guard<std::mutex> guard (mutex);
	items.get<tag_root> ().erase (root_a);
}

std::chrono::steady_clock::time_point ysu::dropped_elections::find (ysu::qualified_root const & root_a) const
{
	ysu::lock_guard<std::mutex> guard (mutex);
	auto & items_by_root = items.get<tag_root> ();
	auto existing (items_by_root.find (root_a));
	if (existing != items_by_root.end ())
	{
		return existing->time;
	}
	else
	{
		return std::chrono::steady_clock::time_point{};
	}
}

size_t ysu::dropped_elections::size () const
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return items.size ();
}