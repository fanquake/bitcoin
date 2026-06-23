// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/system.h>
#include <support/lockedpool.h>

#include <limits>
#include <memory>
#include <stdexcept>
#include <util/byte_units.h>
#include <utility>
#include <vector>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(allocator_tests)

TEST_CASE(arena_tests)
{
    // Fake memory base address for testing
    // without actually using memory.
    void *synth_base = reinterpret_cast<void*>(0x08000000);
    const size_t synth_size{1_MiB};
    Arena b(synth_base, synth_size, 16);
    void *chunk = b.alloc(1000);
#ifdef ARENA_DEBUG
    b.walk();
#endif
    CHECK(chunk != nullptr);
    CHECK(b.stats().used == 1008U); // Aligned to 16
    CHECK(b.stats().total == synth_size); // Nothing has disappeared?
    b.free(chunk);
#ifdef ARENA_DEBUG
    b.walk();
#endif
    CHECK(b.stats().used == 0U);
    CHECK(b.stats().free == synth_size);
    try { // Test exception on double-free
        b.free(chunk);
        CHECK(0);
    } catch(std::runtime_error &)
    {
    }

    void *a0 = b.alloc(128);
    void *a1 = b.alloc(256);
    void *a2 = b.alloc(512);
    CHECK(b.stats().used == 896U);
    CHECK(b.stats().total == synth_size);
#ifdef ARENA_DEBUG
    b.walk();
#endif
    b.free(a0);
#ifdef ARENA_DEBUG
    b.walk();
#endif
    CHECK(b.stats().used == 768U);
    b.free(a1);
    CHECK(b.stats().used == 512U);
    void *a3 = b.alloc(128);
#ifdef ARENA_DEBUG
    b.walk();
#endif
    CHECK(b.stats().used == 640U);
    b.free(a2);
    CHECK(b.stats().used == 128U);
    b.free(a3);
    CHECK(b.stats().used == 0U);
    CHECK(b.stats().chunks_used == 0U);
    CHECK(b.stats().total == synth_size);
    CHECK(b.stats().free == synth_size);
    CHECK(b.stats().chunks_free == 1U);

    std::vector<void*> addr;
    CHECK(b.alloc(0) == nullptr); // allocating 0 always returns nullptr
#ifdef ARENA_DEBUG
    b.walk();
#endif
    // Sweeping allocate all memory
    addr.reserve(2048);
    for (int x=0; x<1024; ++x)
        addr.push_back(b.alloc(1024));
    CHECK(b.stats().free == 0U);
    CHECK(b.alloc(1024) == nullptr); // memory is full, this must return nullptr
    CHECK(b.alloc(0) == nullptr);
    for (int x=0; x<1024; ++x)
        b.free(addr[x]);
    addr.clear();
    CHECK(b.stats().total == synth_size);
    CHECK(b.stats().free == synth_size);

    // Now in the other direction...
    for (int x=0; x<1024; ++x)
        addr.push_back(b.alloc(1024));
    for (int x=0; x<1024; ++x)
        b.free(addr[1023-x]);
    addr.clear();

    // Now allocate in smaller unequal chunks, then deallocate haphazardly
    // Not all the chunks will succeed allocating, but freeing nullptr is
    // allowed so that is no problem.
    for (int x=0; x<2048; ++x)
        addr.push_back(b.alloc(x+1));
    for (int x=0; x<2048; ++x)
        b.free(addr[((x*23)%2048)^242]);
    addr.clear();

    // Go entirely wild: free and alloc interleaved,
    // generate targets and sizes using pseudo-randomness.
    for (int x=0; x<2048; ++x)
        addr.push_back(nullptr);
    uint32_t s = 0x12345678;
    for (int x=0; x<5000; ++x) {
        int idx = s & (addr.size()-1);
        if (s & 0x80000000) {
            b.free(addr[idx]);
            addr[idx] = nullptr;
        } else if(!addr[idx]) {
            addr[idx] = b.alloc((s >> 16) & 2047);
        }
        bool lsb = s & 1;
        s >>= 1;
        if (lsb)
            s ^= 0xf00f00f0; // LFSR period 0xf7ffffe0
    }
    for (void *ptr: addr)
        b.free(ptr);
    addr.clear();

    CHECK(b.stats().total == synth_size);
    CHECK(b.stats().free == synth_size);
}

/** Mock LockedPageAllocator for testing */
class TestLockedPageAllocator: public LockedPageAllocator
{
public:
    TestLockedPageAllocator(int count_in, int lockedcount_in): count(count_in), lockedcount(lockedcount_in) {}
    void* AllocateLocked(size_t len, bool *lockingSuccess) override
    {
        *lockingSuccess = false;
        if (count > 0) {
            --count;

            if (lockedcount > 0) {
                --lockedcount;
                *lockingSuccess = true;
            }

            return reinterpret_cast<void*>(uint64_t{static_cast<uint64_t>(0x08000000) + (count << 24)}); // Fake address, do not actually use this memory
        }
        return nullptr;
    }
    void FreeLocked(void* addr, size_t len) override
    {
    }
    size_t GetLimit() override
    {
        return std::numeric_limits<size_t>::max();
    }
private:
    int count;
    int lockedcount;
};

TEST_CASE(lockedpool_tests_mock)
{
    // Test over three virtual arenas, of which one will succeed being locked
    std::unique_ptr<LockedPageAllocator> x = std::make_unique<TestLockedPageAllocator>(3, 1);
    LockedPool pool(std::move(x));
    CHECK(pool.stats().total == 0U);
    CHECK(pool.stats().locked == 0U);

    // Ensure unreasonable requests are refused without allocating anything
    void *invalid_toosmall = pool.alloc(0);
    CHECK(invalid_toosmall == nullptr);
    CHECK(pool.stats().used == 0U);
    CHECK(pool.stats().free == 0U);
    void *invalid_toobig = pool.alloc(LockedPool::ARENA_SIZE+1);
    CHECK(invalid_toobig == nullptr);
    CHECK(pool.stats().used == 0U);
    CHECK(pool.stats().free == 0U);

    void *a0 = pool.alloc(LockedPool::ARENA_SIZE / 2);
    CHECK(a0);
    CHECK(pool.stats().locked == LockedPool::ARENA_SIZE);
    void *a1 = pool.alloc(LockedPool::ARENA_SIZE / 2);
    CHECK(a1);
    void *a2 = pool.alloc(LockedPool::ARENA_SIZE / 2);
    CHECK(a2);
    void *a3 = pool.alloc(LockedPool::ARENA_SIZE / 2);
    CHECK(a3);
    void *a4 = pool.alloc(LockedPool::ARENA_SIZE / 2);
    CHECK(a4);
    void *a5 = pool.alloc(LockedPool::ARENA_SIZE / 2);
    CHECK(a5);
    // We've passed a count of three arenas, so this allocation should fail
    void *a6 = pool.alloc(16);
    CHECK(!a6);

    pool.free(a0);
    pool.free(a2);
    pool.free(a4);
    pool.free(a1);
    pool.free(a3);
    pool.free(a5);
    CHECK(pool.stats().total == 3*LockedPool::ARENA_SIZE);
    CHECK(pool.stats().locked == LockedPool::ARENA_SIZE);
    CHECK(pool.stats().used == 0U);
}

// These tests used the live LockedPoolManager object, this is also used
// by other tests so the conditions are somewhat less controllable and thus the
// tests are somewhat more error-prone.
TEST_CASE(lockedpool_tests_live)
{
    LockedPoolManager &pool = LockedPoolManager::Instance();
    LockedPool::Stats initial = pool.stats();

    void *a0 = pool.alloc(16);
    CHECK(a0);
    // Test reading and writing the allocated memory
    *((uint32_t*)a0) = 0x1234;
    CHECK(*((uint32_t*)a0) == 0x1234U);

    pool.free(a0);
    try { // Test exception on double-free
        pool.free(a0);
        CHECK(0);
    } catch(std::runtime_error &)
    {
    }
    // If more than one new arena was allocated for the above tests, something is wrong
    CHECK(pool.stats().total <= (initial.total + LockedPool::ARENA_SIZE));
    // Usage must be back to where it started
    CHECK(pool.stats().used == initial.used);
}

TEST_SUITE_END()
