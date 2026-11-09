// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <coinsstore.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

constexpr uint32_t NUM_OUTPOINTS{64};
constexpr size_t WINDOW{4};

COutPoint OutpointAt(uint32_t i)
{
    uint256 h;
    h.data()[0] = static_cast<unsigned char>(i);
    h.data()[1] = 0x55;
    return COutPoint{Txid::FromUint256(h), i % 3};
}

Coin CoinFor(uint32_t i, int height)
{
    return Coin{CTxOut{static_cast<CAmount>(1000 + i), CScript() << std::vector<unsigned char>{static_cast<unsigned char>(i)}}, height, false};
}

struct SynthBlock {
    int height{0};
    std::vector<uint32_t> spends;
    std::vector<uint32_t> creates;
};

} // namespace

/**
 * The window oracle: a valid chain of synthetic blocks is applied to a CoinsStore in whatever order the
 * fuzzer picks inside a window above the confirmed tip, with spends of coins not yet visible deferred to
 * the block's confirmation and blocks occasionally unwound and applied again; blocks confirm in height
 * order. The confirmed state must equal, at every confirmation, that of a CCoinsViewCache that applied the
 * same blocks in order, and the store's bookkeeping must stay consistent.
 */
FUZZ_TARGET(coinsstore_window)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};

    CCoinsViewCache reference{&CoinsViewEmpty::Get()};
    CoinsStore store{&CoinsViewEmpty::Get(), /*shards=*/4, /*deterministic=*/true};
    reference.SetBestBlock(uint256::ONE);
    store.SetBestBlock(uint256::ONE, 0);

    // Seed coins.
    std::vector<uint32_t> live;
    const uint32_t seeded{provider.ConsumeIntegralInRange<uint32_t>(1, 16)};
    for (uint32_t i{0}; i < seeded; ++i) {
        reference.AddCoin(OutpointAt(i), CoinFor(i, 0), false);
        store.AddCoin(OutpointAt(i), CoinFor(i, 0), false);
        live.push_back(i);
    }
    uint32_t next_fresh{seeded};

    // A valid chain, built in order.
    std::vector<SynthBlock> chain;
    const int blocks{provider.ConsumeIntegralInRange<int>(1, 16)};
    for (int h{1}; h <= blocks; ++h) {
        SynthBlock b{.height = h, .spends = {}, .creates = {}};
        const int spends{provider.ConsumeIntegralInRange<int>(0, 3)};
        for (int k{0}; k < spends && !live.empty(); ++k) {
            const size_t idx{provider.ConsumeIntegralInRange<size_t>(0, live.size() - 1)};
            b.spends.push_back(live[idx]);
            live.erase(live.begin() + idx);
        }
        const int creates{provider.ConsumeIntegralInRange<int>(0, 3)};
        for (int k{0}; k < creates && next_fresh < NUM_OUTPOINTS; ++k) {
            b.creates.push_back(next_fresh);
            live.push_back(next_fresh);
            ++next_fresh;
        }
        chain.push_back(std::move(b));
    }

    // Apply out of order, confirm in order, unwind now and then.
    std::vector<CoinsStore::BlockId> ids(chain.size(), CoinsStore::NO_BLOCK);
    std::vector<std::vector<uint32_t>> deferred(chain.size());
    size_t next_to_confirm{0};
    size_t reference_applied{0};
    LIMITED_WHILE(next_to_confirm < chain.size() && provider.remaining_bytes() > 0, 10000)
    {
        std::vector<size_t> unapplied;
        for (size_t i{next_to_confirm}; i < std::min(chain.size(), next_to_confirm + WINDOW); ++i) {
            if (ids[i] == CoinsStore::NO_BLOCK) unapplied.push_back(i);
        }
        const bool lowest_applied{ids[next_to_confirm] != CoinsStore::NO_BLOCK};
        const int action{provider.ConsumeIntegralInRange<int>(0, 9)};
        if (!unapplied.empty() && (action < 5 || !lowest_applied)) {
            const size_t i{unapplied[provider.ConsumeIntegralInRange<size_t>(0, unapplied.size() - 1)]};
            const SynthBlock& b{chain[i]};
            ids[i] = store.BeginBlock(b.height);
            deferred[i].clear();
            for (uint32_t c : b.creates) store.ApplyCreate(ids[i], OutpointAt(c), CoinFor(c, b.height), false);
            for (uint32_t sp : b.spends) {
                if (store.LookupFor(ids[i], OutpointAt(sp))) {
                    assert(store.ApplySpend(ids[i], OutpointAt(sp), nullptr));
                } else {
                    deferred[i].push_back(sp);
                }
            }
        } else if (action < 7 && lowest_applied) {
            // Unwind a random applied, unconfirmed block (the oracle's "reorg inside the window"): every
            // applied block above it is unwound too, since its results may rest on it.
            std::vector<size_t> applied;
            for (size_t i{next_to_confirm}; i < chain.size() && i < next_to_confirm + WINDOW; ++i) {
                if (ids[i] != CoinsStore::NO_BLOCK) applied.push_back(i);
            }
            const size_t from{applied[provider.ConsumeIntegralInRange<size_t>(0, applied.size() - 1)]};
            for (size_t i{chain.size()}; i-- > from;) {
                if (ids[i] != CoinsStore::NO_BLOCK) {
                    store.UnwindBlock(ids[i]);
                    ids[i] = CoinsStore::NO_BLOCK;
                }
            }
        } else if (lowest_applied) {
            const size_t i{next_to_confirm};
            for (uint32_t sp : deferred[i]) {
                assert(store.LookupFor(ids[i], OutpointAt(sp)).has_value());
                assert(store.ApplySpend(ids[i], OutpointAt(sp), nullptr));
            }
            assert(store.ConflictCount(ids[i]) == 0);
            store.ConfirmBlock(ids[i]);
            ++next_to_confirm;
            // The reference catches up to the same height.
            while (reference_applied < next_to_confirm) {
                const SynthBlock& b{chain[reference_applied]};
                for (uint32_t c : b.creates) reference.AddCoin(OutpointAt(c), CoinFor(c, b.height), false);
                for (uint32_t sp : b.spends) assert(reference.SpendCoin(OutpointAt(sp)));
                ++reference_applied;
            }
            // Confirmed state: identical.
            for (uint32_t i{0}; i < NUM_OUTPOINTS; ++i) {
                const auto a{reference.PeekCoin(OutpointAt(i))};
                const auto b{store.PeekCoin(OutpointAt(i))};
                assert(a.has_value() == b.has_value());
                if (a) assert(a->out == b->out && a->nHeight == b->nHeight);
            }
            store.SanityCheck();
        }
    }
    store.SanityCheck();
}
