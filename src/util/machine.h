// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_MACHINE_H
#define BITCOIN_UTIL_MACHINE_H

#include <array>
#include <cstddef>
#include <optional>

namespace util {
//! One edge of a state machine: in state `from`, `event` leads to state `to`.
template <typename State, typename Event>
struct Edge {
    State from;
    Event event;
    State to;
};

/**
 * The transition table of a state machine, fixed at compile time.
 *
 * A machine is an enum of states, an enum of events and one table of edges.
 * A (state, event) pair has at most one edge, which the constructor checks
 * during compilation, so a table that names two outcomes for one pair does
 * not build. A pair without an edge is a refused transition: Next() returns
 * nullopt and the caller decides what a refusal means at its site.
 *
 * The table holds no state of its own. A transition takes the state by value
 * and returns the next one; whatever the transition causes outside the
 * machine is the caller's business and happens after the table agreed.
 */
template <typename State, typename Event, size_t N>
class TransitionTable
{
    std::array<Edge<State, Event>, N> m_edges{};

public:
    consteval explicit TransitionTable(const Edge<State, Event> (&edges)[N])
    {
        for (size_t i{0}; i < N; ++i) {
            for (size_t j{0}; j < i; ++j) {
                if (edges[i].from == edges[j].from && edges[i].event == edges[j].event) {
                    throw "two edges for one (state, event) pair";
                }
            }
            m_edges[i] = edges[i];
        }
    }

    //! The state `event` leads to from `from`, or nullopt if the machine refuses it.
    constexpr std::optional<State> Next(State from, Event event) const
    {
        for (const auto& edge : m_edges) {
            if (edge.from == from && edge.event == event) return edge.to;
        }
        return std::nullopt;
    }

    //! Whether `event` leads from `from` to exactly `to`.
    constexpr bool Allows(State from, Event event, State to) const
    {
        const auto next{Next(from, event)};
        return next && *next == to;
    }
};

//! Build a table from a braced list of edges: MakeTransitionTable<State, Event>({{A, E, B}, ...}).
template <typename State, typename Event, size_t N>
consteval TransitionTable<State, Event, N> MakeTransitionTable(const Edge<State, Event> (&edges)[N])
{
    return TransitionTable<State, Event, N>{edges};
}
} // namespace util

#endif // BITCOIN_UTIL_MACHINE_H
