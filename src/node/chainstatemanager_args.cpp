// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/chainstatemanager_args.h>

#include <arith_uint256.h>
#include <common/args.h>
#include <common/system.h>
#include <node/coins_view_args.h>
#include <node/database_args.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/log.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <validation.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

namespace node {
int32_t DefaultPrevoutFetchThreads()
{
    return std::clamp<int32_t>(GetNumCores(), MIN_DEFAULT_PREVOUTFETCH_THREADS, MAX_PREVOUTFETCH_THREADS);
}

size_t DefaultBlockCacheBytes(size_t coins_cache_bytes)
{
    return std::clamp<size_t>(coins_cache_bytes, 32_MiB, DEFAULT_BLOCK_CACHE_BYTES);
}

util::Result<void> ApplyArgsManOptions(const ArgsManager& args, ChainstateManager::Options& opts)
{
    if (auto value{args.GetIntArg("-checkblockindex")}) {
        // Interpret bare -checkblockindex argument as 1 instead of 0.
        opts.check_block_index = args.GetArg("-checkblockindex")->empty() ? 1 : *value;
    }

    if (auto value{args.GetArg("-minimumchainwork")}) {
        if (auto min_work{uint256::FromUserHex(*value)}) {
            opts.minimum_chain_work = UintToArith256(*min_work);
        } else {
            return util::Error{Untranslated(strprintf("Invalid minimum work specified (%s), must be up to %d hex digits", *value, uint256::size() * 2))};
        }
    }

    if (auto value{args.GetArg("-assumevalid")}) {
        if (auto block_hash{uint256::FromUserHex(*value)}) {
            opts.assumed_valid_block = *block_hash;
        } else {
            return util::Error{Untranslated(strprintf("Invalid assumevalid block hash specified (%s), must be up to %d hex digits (or 0 to disable)", *value, uint256::size() * 2))};
        }
    }

    if (auto value{args.GetIntArg("-maxtipage")}) opts.max_tip_age = std::chrono::seconds{*value};

    ReadDatabaseArgs(args, opts.coins_db);
    ReadCoinsViewArgs(args, opts.coins_view);

    int script_threads = args.GetIntArg("-par", DEFAULT_SCRIPTCHECK_THREADS);
    if (script_threads <= 0) {
        // -par=0 means autodetect (number of cores - 1 script threads)
        // -par=-n means "leave n cores free" (number of cores - n - 1 script threads)
        script_threads += GetNumCores();
    }
    // Subtract 1 because the main thread counts towards the par threads.
    opts.worker_threads_num = script_threads - 1;

    opts.test_write_out_every_block = HasTestOption(args, "write_out_every_block");

    if (auto value{args.GetArg<int64_t>("-blockcache")}) {
        if (*value < 0) {
            return util::Error{Untranslated(strprintf("-blockcache must be non-negative (got %d). Use 0 to disable the block cache.", *value))};
        }
        // Capped at what a size_t counts in bytes, as -dbcache is.
        opts.block_cache_bytes = static_cast<size_t>(std::min<uint64_t>(*value, std::numeric_limits<size_t>::max() >> 20)) << 20;
    }

    if (auto value{args.GetArg<int32_t>("-prevoutfetchthreads")}) {
        if (*value < 0) {
            return util::Error{Untranslated(strprintf("-prevoutfetchthreads must be non-negative (got %d). Use 0 for no connect pool threads.", *value))};
        }
        opts.prevoutfetch_threads_num = std::min(*value, MAX_PREVOUTFETCH_THREADS);
    } else {
        opts.prevoutfetch_threads_num = DefaultPrevoutFetchThreads();
    }

    if (args.IsArgNegated("-connectwindow")) {
        opts.connect_window = 1; // -noconnectwindow: one block at a time
    } else if (auto value{args.GetArg<int32_t>("-connectwindow")}) {
        if (*value < 0) {
            return util::Error{Untranslated(strprintf("-connectwindow must be non-negative (got %d). Use 0 for the default, 1 to validate one block at a time.", *value))};
        }
        opts.connect_window = std::min(*value, MAX_CONNECT_WINDOW);
    }

    if (auto max_size = args.GetIntArg("-maxsigcachesize")) {
        // 1. When supplied with a max_size of 0, both the signature cache and
        //    script execution cache create the minimum possible cache (2
        //    elements). Therefore, we can use 0 as a floor here.
        // 2. Multiply first, divide after to avoid integer truncation.
        size_t clamped_size_each{size_t(std::max<int64_t>(*max_size, 0) * 1_MiB / 2)};
        opts.script_execution_cache_bytes = clamped_size_each;
        opts.signature_cache_bytes = clamped_size_each;
    }

    return {};
}
} // namespace node
