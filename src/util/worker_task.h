// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_WORKER_TASK_H
#define BITCOIN_UTIL_WORKER_TASK_H

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace util {
/**
 * A kind of task that runs on a worker thread, where there is no caller on
 * the stack to decide what a std::runtime_error means: its name for the log.
 * This is for the tasks run through RunWorkerTask(), whose failure is one
 * the node cannot go on after (a failed read or write of its own data): the
 * owner of the task raises a fatal error. A task that expects an error of
 * its own handles that one itself, and lets no other leave it.
 */
struct WorkerTaskType {
    std::string_view name;
};

/** A worker task's failure as a value, for the thread that owns the task to act on. */
struct WorkerTaskFailure {
    std::string what;
};

/**
 * Run a task of the given type on the calling (worker) thread.
 *
 * A std::runtime_error, or a class derived from it such as a failed read
 * of a file, is returned as a value. Nothing wider is caught: a logic error is a bug and a failed
 * allocation has no handler anywhere in the node, and both terminate.
 */
template <typename Task>
[[nodiscard]] std::optional<WorkerTaskFailure> RunWorkerTask(const WorkerTaskType& type, Task&& task)
{
    try {
        std::forward<Task>(task)();
    } catch (const std::runtime_error& e) {
        return WorkerTaskFailure{std::string{type.name} + ": " + e.what()};
    }
    return std::nullopt;
}
} // namespace util

#endif // BITCOIN_UTIL_WORKER_TASK_H
