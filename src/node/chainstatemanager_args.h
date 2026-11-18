// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CHAINSTATEMANAGER_ARGS_H
#define BITCOIN_NODE_CHAINSTATEMANAGER_ARGS_H

#include <util/result.h>
#include <validation.h>

class ArgsManager;

/** -par default (number of script-checking threads, 0 = auto) */
inline constexpr int DEFAULT_SCRIPTCHECK_THREADS{0};

/** The fewest connect pool threads -prevoutfetchthreads defaults to: with a single one the blocks of a peer
 *  stay on the message handler. */
inline constexpr int32_t MIN_DEFAULT_PREVOUTFETCH_THREADS{2};

namespace node {
/** The default of -prevoutfetchthreads: one connect pool thread for each core, at least
 *  MIN_DEFAULT_PREVOUTFETCH_THREADS and at most MAX_PREVOUTFETCH_THREADS. Threads beyond the cores only hold
 *  memory: by default the window of blocks in flight is twice as wide as the pool, up to MAX_CONNECT_WINDOW. */
int32_t DefaultPrevoutFetchThreads();
/** The default of -blockcache for a coins cache of `coins_cache_bytes`: DEFAULT_BLOCK_CACHE_BYTES, but no more
 *  than the coins cache itself and at least 32 MiB. The blocks it holds are on disk, and a block not found in it
 *  is read back by the worker that validates it, so beside a small coins cache a large block cache holds memory
 *  the node was told not to use and gains nothing for it. */
size_t DefaultBlockCacheBytes(size_t coins_cache_bytes);
[[nodiscard]] util::Result<void> ApplyArgsManOptions(const ArgsManager& args, ChainstateManager::Options& opts);
} // namespace node

#endif // BITCOIN_NODE_CHAINSTATEMANAGER_ARGS_H
