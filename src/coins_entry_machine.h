// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINS_ENTRY_MACHINE_H
#define BITCOIN_COINS_ENTRY_MACHINE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

/**
 * The state of one coin entry in the coins store (CoinsStore::Entry), and
 * every way it changes.
 *
 * The state is a set of bits: seven flags the entry stores in one byte, and
 * whether its coin is spent, which the coin says. Few of the 256 sets mean
 * anything: IsStored() names those an entry of the store may rest in, and
 * ABSENT is the entry that was just inserted and holds nothing yet.
 *
 * An event is something the store does to an entry: a confirmed change, an
 * open block's create or spend, a block's unwind, a write-out run collecting
 * the entry or landing. RULES lists, per event, the states it applies to and
 * what it clears and sets. NEXT is that list as a table from (event, state)
 * to the next state, built during compilation, which refuses to build if
 *  - two rules of an event apply to one state, or
 *  - a rule leads from a state an entry may be in to one it may not be in.
 * The set of states is therefore closed under the events, by construction.
 * ABSENT is one of them, and a rule may lead there (a run that lands on an
 * entry with nothing left to say): that such an entry leaves the map is the
 * store's to do (SettleNode()), not the table's.
 * An event in a state that none of its rules names is refused (REFUSED): the
 * store's logic went wrong before, and the store stops there.
 *
 * An entry is also filed under a height (CoinsStore::Entry::height), which
 * decides which write-out run carries it. Each rule says what its event does
 * to that height (Height), and NextHeight() is the one place the answer is
 * worked out. What the rules keep, between them:
 *
 *   An entry that owes the database an erase is filed at or below the height
 *   the erase is owed since, so that a run bounded at that height or above
 *   writes it. The one exception is the spent marker of an open block, filed
 *   at the block's height while the block is open, with the lower height in
 *   the block's record; the block's confirmation (BlockSpendConfirmed) or its
 *   unwind puts the entry back under it.
 *
 * The machine holds no entry. The store changes an entry's flags and its
 * height through CoinsStore::Transit(), which asks NEXT and NextHeight(), and
 * CoinsStore::Enter(), with what they gave; erasing an entry is the one change
 * that is not an event, since nothing is left to have a state.
 */
namespace coins_entry {
using State = uint8_t;

//! The entry differs from the database: it is on a dirty list, for a write-out run to find.
inline constexpr State DIRTY{1 << 0};
//! The database is known not to hold the outpoint, so a spend leaves nothing to erase.
inline constexpr State FRESH{1 << 1};
//! A run collected the entry's value and has not settled it; any change clears the bit, and the entry
//! then stays dirty when the run's chunk settles.
inline constexpr State COLLECTED{1 << 2};
//! An unspent coin a block created, at the coin's height: pending while the confirmed height is below it.
inline constexpr State BLOCK_CREATE{1 << 3};
//! A spent entry a block's spend left for the erase the database is owed, the change height being the
//! block's: pending while the confirmed height is below it, the coin in the block's record.
inline constexpr State BLOCK_SPEND{1 << 4};
//! With BLOCK_CREATE: the create went over a change the database is owed still, whose change height the
//! entry keeps. A run that passes writes that change and moves the entry to the coin's height.
inline constexpr State OWED{1 << 5};
//! With BLOCK_SPEND: the spend was of a coin the database never held, and left the entry for other open
//! blocks alone. There is no erase to write. A change that puts a coin in the entry clears the bit, and so
//! does a run that writes the entry; another block's takeover of the spend, the spend's confirmation and a
//! flush keep it.
inline constexpr State UNWRITTEN{1 << 6};
//! The coin is spent. Not stored with the flags: the coin says it.
inline constexpr State SPENT{1 << 7};

//! The flags an entry stores.
inline constexpr State FLAGS{DIRTY | FRESH | COLLECTED | BLOCK_CREATE | BLOCK_SPEND | OWED | UNWRITTEN};
//! An entry just inserted: no coin and nothing to say. Every insertion is followed by an event.
inline constexpr State ABSENT{SPENT};
//! A clean, unspent copy of what the database holds.
inline constexpr State BASE_COPY{0};
//! No state: what NEXT answers for an event the machine refuses.
inline constexpr State REFUSED{0xff};

//! Whether an entry of the store may rest in `s`. The block flags (BLOCK_CREATE, BLOCK_SPEND, OWED,
//! UNWRITTEN) outlive their block's confirmation and go when the entry is written; whether they still
//! refer to an open block is a matter of heights, which the machine does not see.
constexpr bool IsStored(State s)
{
    const bool dirty(s & DIRTY), fresh(s & FRESH), collected(s & COLLECTED), block_create(s & BLOCK_CREATE);
    const bool block_spend(s & BLOCK_SPEND), owed(s & OWED), unwritten(s & UNWRITTEN), spent(s & SPENT);
    // A clean entry is an unspent copy of the database's coin and nothing else: a spent entry is only
    // ever kept to record an erase, and what a block or a run left on an entry goes when it is written.
    if (!dirty) return s == BASE_COPY;
    // A fresh entry is a coin, no block's spend, and neither on its way to the database nor over
    // something the database is owed.
    if (fresh && (spent || collected || block_spend || owed)) return false;
    // A block's create is a coin, a block's spend is none.
    if (block_create && (spent || block_spend)) return false;
    if (block_spend && !spent) return false;
    // Something is owed under a block's create only.
    if (owed && !block_create) return false;
    // Only a block's spent marker says the database never held its coin, and no run takes such a marker.
    if (unwritten && (!block_spend || collected)) return false;
    return true;
}

//! Whether the machine knows `s`: a state an entry may rest in, or the one it is inserted in.
constexpr bool IsKnown(State s) { return s == ABSENT || IsStored(s); }

/**
 * What a lookup has of an entry, as far as its state says. For a block's create and a block's spend the
 * heights say the rest (CoinsStore::See()): where the viewer stands, and whether the block is confirmed.
 * Nothing else about the entry decides what is seen: not whether it is dirty or fresh, nor what a run
 * or the database is owed.
 */
enum class Reading : uint8_t {
    //! Spent, by a confirmed spend.
    NOTHING,
    //! A confirmed coin.
    COIN,
    //! A block's create: the coin, for a viewer at or above the block and for everyone once the block is
    //! confirmed; nothing until then for a reader and for a lower block.
    BLOCK_COIN,
    //! A block's spend: nothing, for a viewer at or above the block and for everyone once the block is
    //! confirmed; until then a reader and a lower block see the coin, which is in the block's record.
    BLOCK_RECORD,
};
constexpr Reading ReadingOf(State s)
{
    if (s & SPENT) return s & BLOCK_SPEND ? Reading::BLOCK_RECORD : Reading::NOTHING;
    return s & BLOCK_CREATE ? Reading::BLOCK_COIN : Reading::COIN;
}

enum class Event : uint8_t {
    //! A lookup brings in the database's coin.
    BaseCopy,
    //! A confirmed coin the database is known not to hold.
    ConfirmedNew,
    //! A confirmed coin written over whatever was there, of which the database may hold something.
    ConfirmedWrite,
    //! As ConfirmedWrite, from a child view's batch over an entry that was there: one the database is
    //! known not to hold stays so.
    BatchOverwrite,
    //! A confirmed spend of an entry the database may hold: the erase is to be written.
    ConfirmedSpend,
    //! A run wrote a coin a disconnect restored: the database holds the outpoint now.
    KeptWritten,
    //! An open block's create where the tier and the database are known to hold nothing.
    BlockCreateFresh,
    //! An open block's create where the tier held nothing and the database may (a possible overwrite).
    BlockCreateBlind,
    //! A lower open block's create takes over the entry of a higher block's create of the same outpoint.
    BlockCreateTakeover,
    //! As BlockCreateTakeover, where the lower block's create may overwrite a coin of the database.
    BlockCreateTakeoverBlind,
    //! An open block's create over a confirmed entry: a dirty one leaves its change owed under the create.
    BlockCreateOnConfirmed,
    //! An open block's spend of a coin only the database holds leaves a marker (also a run that wrote the
    //! coin of such a spend from the block's record).
    BlockSpendMarker,
    //! An open block's spend of the coin the entry holds.
    BlockSpend,
    //! A lower open block takes over the marker of a higher block's spend.
    BlockSpendTakeover,
    //! A block's create is unwound and the dirty confirmed coin it overwrote is put back.
    UnwindCreateToCoin,
    //! A block's create is unwound and the erase it went over is owed still.
    UnwindCreateToErase,
    //! A block's spend is unwound, its coin went with the block that created it, and the erase that
    //! create went over is owed still.
    UnwindSpendToErase,
    //! A block is confirmed whose spend took an entry that owed an earlier change: the entry, the
    //! spend's marker or what a higher block's create or its unwind made of it since, goes back under
    //! the height of that change.
    BlockSpendConfirmed,
    //! A run wrote what was owed under a block's create.
    RunWritesOwed,
    //! A run wrote, from a block's record, the coin a marker's spend took: the database holds it now.
    RunWritesRecord,
    //! A run collects the entry's value for the database.
    RunCollects,
    //! The run's chunk is in the database and the entry was not changed since.
    RunLands,
    //! A flush of the whole tier passes the entry and keeps it (an open block's): no run is to settle it.
    Flush,
};
inline constexpr size_t EVENT_COUNT{static_cast<size_t>(Event::Flush) + 1};

/** What an event does to the height an entry is filed under. The event comes with a height of its own. */
enum class Height : uint8_t {
    //! The entry stays where it is filed; the event's height, if it has one, is not looked at.
    Keep,
    //! The entry is filed under the event's height: the change's, the block's, or the coin's.
    Take,
    //! As Take, unless the entry owes an earlier change (OWED) and is filed lower: it stays there, since
    //! that change is still to be written as of its own height.
    Lower,
    //! The entry goes back under the height a block's record kept for it from before the block's change.
    //! That height is never above where the block's change filed the entry.
    Restore,
};

/**
 * In a state that has every bit of `all_of` and none of `none_of`, `event` clears the bits of `clear`
 * and then sets those of `set`, and does `height` to the height the entry is filed under. A rule states
 * SPENT like any other bit: what the coin must be before, and what the store makes of it with the event.
 */
struct Rule {
    Event event;
    State all_of;
    State none_of;
    State clear;
    State set;
    Height height;
};

inline constexpr State EVERYTHING{0xff};

// clang-format off
inline constexpr Rule RULES[]{
    // event                          all_of                        none_of        clear                set                         height
    {Event::BaseCopy,                 SPENT,                        FLAGS,         EVERYTHING,          BASE_COPY,                  Height::Take},
    {Event::ConfirmedNew,             SPENT,                        FLAGS,         EVERYTHING,          DIRTY | FRESH,              Height::Take},
    {Event::ConfirmedWrite,           0,                            0,             EVERYTHING,          DIRTY,                      Height::Lower},
    {Event::BatchOverwrite,           DIRTY,                        0,             EVERYTHING & ~FRESH, DIRTY,                      Height::Lower},
    {Event::BatchOverwrite,           0,                            DIRTY | SPENT, EVERYTHING,          DIRTY,                      Height::Lower},
    {Event::ConfirmedSpend,           0,                            FRESH,         EVERYTHING,          DIRTY | SPENT,              Height::Lower},
    {Event::KeptWritten,              0,                            0,             FRESH,               0,                          Height::Keep},
    {Event::BlockCreateFresh,         SPENT,                        FLAGS,         EVERYTHING,          DIRTY | FRESH | BLOCK_CREATE,Height::Take},
    {Event::BlockCreateBlind,         SPENT,                        FLAGS,         EVERYTHING,          DIRTY | BLOCK_CREATE,       Height::Take},
    {Event::BlockCreateTakeover,      DIRTY | BLOCK_CREATE,         0,             COLLECTED,           0,                          Height::Lower},
    {Event::BlockCreateTakeoverBlind, DIRTY | BLOCK_CREATE,         0,             COLLECTED | FRESH,   0,                          Height::Lower},
    {Event::BlockCreateOnConfirmed,   DIRTY,                        0,             EVERYTHING,          DIRTY | BLOCK_CREATE | OWED,Height::Keep},
    {Event::BlockCreateOnConfirmed,   0,                            DIRTY | SPENT, EVERYTHING,          DIRTY | BLOCK_CREATE,       Height::Take},
    {Event::BlockSpendMarker,         SPENT,                        FLAGS,         EVERYTHING,          DIRTY | BLOCK_SPEND | SPENT,Height::Take},
    {Event::BlockSpend,               FRESH,                        0,             EVERYTHING,          DIRTY | BLOCK_SPEND | SPENT | UNWRITTEN,Height::Take},
    {Event::BlockSpend,               0,                            FRESH | SPENT, EVERYTHING,          DIRTY | BLOCK_SPEND | SPENT,Height::Take},
    {Event::BlockSpendTakeover,       DIRTY | BLOCK_SPEND,          0,             COLLECTED,           0,                          Height::Take},
    {Event::UnwindCreateToCoin,       DIRTY | BLOCK_CREATE,         0,             EVERYTHING,          DIRTY,                      Height::Restore},
    {Event::UnwindCreateToErase,      DIRTY | BLOCK_CREATE | OWED,  0,             EVERYTHING,          DIRTY | SPENT,              Height::Keep},
    {Event::UnwindSpendToErase,       DIRTY | BLOCK_SPEND,          0,             EVERYTHING,          DIRTY | SPENT,              Height::Restore},
    {Event::BlockSpendConfirmed,      DIRTY | SPENT,                COLLECTED,     0,                   0,                          Height::Restore},
    {Event::BlockSpendConfirmed,      DIRTY | BLOCK_CREATE | OWED,  COLLECTED,     0,                   0,                          Height::Restore},
    {Event::RunWritesOwed,            OWED,                         0,             OWED,                0,                          Height::Take},
    {Event::RunWritesRecord,          0,                            0,             UNWRITTEN,           0,                          Height::Keep},
    {Event::RunCollects,              DIRTY,                        UNWRITTEN,     FRESH,               COLLECTED,                  Height::Keep},
    {Event::RunLands,                 DIRTY | COLLECTED,            0,             DIRTY | COLLECTED | BLOCK_CREATE | BLOCK_SPEND | OWED, 0,Height::Keep},
    {Event::Flush,                    0,                            0,             COLLECTED,           0,                          Height::Keep},
};
// clang-format on

using NextTable = std::array<std::array<State, 256>, EVENT_COUNT>;
using HeightTable = std::array<std::array<Height, 256>, EVENT_COUNT>;

consteval NextTable MakeNextTable()
{
    NextTable table{};
    for (auto& row : table) {
        for (State& next : row) next = REFUSED;
    }
    for (const Rule& rule : RULES) {
        auto& row{table[static_cast<size_t>(rule.event)]};
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            if (!IsKnown(from)) continue;
            if ((from & rule.all_of) != rule.all_of || (from & rule.none_of) != 0) continue;
            if (row[s] != REFUSED) throw "two rules of one event apply to one state";
            const State to{static_cast<State>((from & ~rule.clear) | rule.set)};
            if (!IsKnown(to)) throw "a rule leads out of the states an entry may be in";
            row[s] = to;
        }
    }
    return table;
}

//! The state `event` leads to from `from`, or REFUSED.
inline constexpr NextTable NEXT{MakeNextTable()};
static_assert(!IsKnown(REFUSED));

constexpr State Next(State from, Event event) { return NEXT[static_cast<size_t>(event)][from]; }

consteval HeightTable MakeHeightTable()
{
    HeightTable table{};
    for (auto& row : table) {
        for (Height& height : row) height = Height::Keep;
    }
    for (const Rule& rule : RULES) {
        auto& row{table[static_cast<size_t>(rule.event)]};
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            if (!IsKnown(from)) continue;
            if ((from & rule.all_of) != rule.all_of || (from & rule.none_of) != 0) continue;
            row[s] = rule.height;
        }
    }
    return table;
}

//! What `event` does in `from` to the height the entry is filed under. Keep where NEXT refuses.
inline constexpr HeightTable HEIGHT{MakeHeightTable()};

constexpr Height HeightRule(State from, Event event) { return HEIGHT[static_cast<size_t>(event)][from]; }

/**
 * The height an entry is filed under after an event: `rule` is what the event does (HeightRule()), `from`
 * the state before it, `filed` the height the entry is filed under now (which means nothing for an entry
 * just inserted) and `event_height` the event's. Nothing where the rule is broken: a height put back
 * that is above where the entry is filed.
 */
constexpr std::optional<int> NextHeight(Height rule, State from, int filed, int event_height)
{
    switch (rule) {
    case Height::Keep: return filed;
    case Height::Take: return event_height;
    case Height::Lower: return from & OWED ? std::min(filed, event_height) : event_height;
    case Height::Restore:
        if (from != ABSENT && event_height > filed) return std::nullopt;
        return event_height;
    } // no default case, so the compiler can warn about missing cases
    return std::nullopt;
}

/**
 * The one change that is no rule: the unwind of a block's spend puts the entry back as the block's
 * record says it was before the spend. The entry is the spend's marker, or was erased by the spend and
 * is inserted again; what is put back is a coin, and what it may carry is what a coin the block could
 * spend may have carried. Its height is the record's as well (UNWIND_SPEND_RESTORE_HEIGHT).
 */
constexpr State NextUnwindSpendRestore(State from, State recorded)
{
    const bool marker{(from & (DIRTY | BLOCK_SPEND | SPENT)) == (DIRTY | BLOCK_SPEND | SPENT) && IsStored(from)};
    if (from != ABSENT && !marker) return REFUSED;
    if ((recorded & ~(DIRTY | FRESH | BLOCK_CREATE | OWED)) != 0 || !IsStored(recorded)) return REFUSED;
    return recorded;
}
//! What that change does to the height the entry is filed under.
inline constexpr Height UNWIND_SPEND_RESTORE_HEIGHT{Height::Restore};
} // namespace coins_entry

#endif // BITCOIN_COINS_ENTRY_MACHINE_H
