// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <util/threadpool.h>

#include <chrono>
#include <functional>
#include <future>
#include <thread>
#include <vector>

// The pool's hand-off costs. Wall time per operation is what nanobench prints; the CPU these operations burn,
// which is where waking idle workers shows, is the user and system time of the bench process, so run these
// under `time` as well. Workers that idle longer than a few hundred microseconds have gone to sleep on the
// condition variable, as they do between two blocks, which is what the pause before each submission models.

namespace {

constexpr int WORKERS{8};
constexpr auto PAUSE{std::chrono::microseconds{300}};

//! Sleeping rather than spinning, so that the process's CPU time is the pool's and not the pause's.
void Pause()
{
    std::this_thread::sleep_for(PAUSE);
}

//! A couple of tasks on a pool with many workers, after the workers went idle: a small block's hashing or
//! prevout fetching. Only as many workers as there are tasks should wake.
void ThreadPoolFewTasksAfterIdle(benchmark::Bench& bench)
{
    ThreadPool pool{"bench"};
    pool.Start(WORKERS);
    bench.unit("submit").run([&] {
        Pause();
        std::vector<std::function<void()>> tasks(2, [] {});
        auto futures{pool.Submit(std::move(tasks))};
        for (auto& f : *futures) f.wait();
    });
}

//! One task per worker, after the workers went idle: a large block's prevout fetching.
void ThreadPoolAllWorkersAfterIdle(benchmark::Bench& bench)
{
    ThreadPool pool{"bench"};
    pool.Start(WORKERS);
    bench.unit("submit").run([&] {
        Pause();
        std::vector<std::function<void()>> tasks(WORKERS, [] {});
        auto futures{pool.Submit(std::move(tasks))};
        for (auto& f : *futures) f.wait();
    });
}

//! Single tasks back to back, the workers never idle: a block's hashing chunks arriving as they are parsed.
void ThreadPoolSingleTasksHot(benchmark::Bench& bench)
{
    ThreadPool pool{"bench"};
    pool.Start(WORKERS);
    bench.unit("submit").run([&] {
        auto future{pool.Submit([] {})};
        future->wait();
    });
}

} // namespace

BENCHMARK(ThreadPoolFewTasksAfterIdle);
BENCHMARK(ThreadPoolAllWorkersAfterIdle);
BENCHMARK(ThreadPoolSingleTasksHot);
