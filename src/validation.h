// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VALIDATION_H
#define BITCOIN_VALIDATION_H

#include <arith_uint256.h>
#include <attributes.h>
#include <chain.h>
#include <checkqueue.h>
#include <coins.h>
#include <coinsstore.h>
#include <consensus/amount.h>
#include <cuckoocache.h>
#include <deploymentstatus.h>
#include <kernel/chain.h>
#include <kernel/chainparams.h>
#include <kernel/chainstatemanager_opts.h>
#include <kernel/cs_main.h> // IWYU pragma: export
#include <node/blockcache.h>
#include <node/blockstorage.h>
#include <node/handoff_machine.h>
#include <node/sync_mode.h>
#include <node/tip_snapshot.h>
#include <policy/feerate.h>
#include <policy/packages.h>
#include <policy/policy.h>
#include <script/script_error.h>
#include <script/sigcache.h>
#include <script/verify_flags.h>
#include <sync.h>
#include <txdb.h>
#include <txmempool.h>
#include <uint256.h>
#include <undo.h>
#include <util/byte_units.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/hasher.h>
#include <util/byte_ledger.h>
#include <util/lifecycle.h>
#include <util/result.h>
#include <util/time.h>
#include <util/translation.h>
#include <versionbits.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class BlockBins;
class Chainstate;
class CTxMemPool;
class ChainstateManager;
struct ChainTxData;
class DisconnectedBlockTransactions;
struct PrecomputedTransactionData;
struct LockPoints;
struct AssumeutxoData;
namespace kernel {
struct ChainstateRole;
} // namespace kernel
namespace node {
class SnapshotMetadata;
} // namespace node
namespace Consensus {
struct Params;
} // namespace Consensus
namespace util {
class SignalInterrupt;
} // namespace util

/** Block files containing a block-height within MIN_BLOCKS_TO_KEEP of ActiveChain().Tip() will not be pruned. */
inline constexpr unsigned int MIN_BLOCKS_TO_KEEP = 288;
inline constexpr signed int DEFAULT_CHECKBLOCKS = 6;
inline constexpr int DEFAULT_CHECKLEVEL{3};
// Require that user allocate at least 550 MiB for block & undo files (blk???.dat and rev???.dat)
// At 1MB per block, 288 blocks = 288MB.
// Add 15% for Undo data = 331MB
// Add 20% for Orphan block rate = 397MB
// We want the low water mark after pruning to be at least 397 MB and since we prune in
// full block file chunks, we need the high water mark which triggers the prune to be
// one 128MB block file + added 15% undo data = 147MB greater for a total of 545MB
// Setting the target to >= 550 MiB will make it likely we can respect the target.
inline constexpr uint64_t MIN_DISK_SPACE_FOR_BLOCK_FILES{550_MiB};

/** Maximum number of dedicated script-checking threads allowed */
inline constexpr int MAX_SCRIPTCHECK_THREADS{15};

/** Maximum number of connect pool threads (-prevoutfetchthreads) */
inline constexpr int32_t MAX_PREVOUTFETCH_THREADS{1024};
//! The most blocks the connect window validates at once (-connectwindow).
inline constexpr int MAX_CONNECT_WINDOW{64};

/** Current sync state passed to tip changed callbacks. */
enum class SynchronizationState {
    INIT_REINDEX,
    INIT_DOWNLOAD,
    POST_INIT
};

/** Documentation for argument 'checklevel'. */
extern const std::vector<std::string> CHECKLEVEL_DOC;

CAmount GetBlockSubsidy(int nHeight, const Consensus::Params& consensusParams);

bool FatalError(kernel::Notifications& notifications, BlockValidationState& state, const bilingual_str& message);

/** Prune block files up to a given height */
void PruneBlockFilesManual(Chainstate& active_chainstate, int nManualPruneHeight);

/**
* Validation result for a transaction evaluated by MemPoolAccept (single or package).
* Here are the expected fields and properties of a result depending on its ResultType, applicable to
* results returned from package evaluation:
*+---------------------------+----------------+-------------------+------------------+----------------+-------------------+
*| Field or property         |    VALID       |                 INVALID              |  MEMPOOL_ENTRY | DIFFERENT_WITNESS |
*|                           |                |--------------------------------------|                |                   |
*|                           |                | TX_RECONSIDERABLE |     Other        |                |                   |
*+---------------------------+----------------+-------------------+------------------+----------------+-------------------+
*| txid in mempool?          | yes            | no                | no*              | yes            | yes               |
*| wtxid in mempool?         | yes            | no                | no*              | yes            | no                |
*| m_state                   | yes, IsValid() | yes, IsInvalid()  | yes, IsInvalid() | yes, IsValid() | yes, IsValid()    |
*| m_vsize                   | yes            | no                | no               | yes            | no                |
*| m_base_fees               | yes            | no                | no               | yes            | no                |
*| m_effective_feerate       | yes            | yes               | no               | no             | no                |
*| m_wtxids_fee_calculations | yes            | yes               | no               | no             | no                |
*| m_other_wtxid             | no             | no                | no               | no             | yes               |
*+---------------------------+----------------+-------------------+------------------+----------------+-------------------+
* (*) Individual transaction acceptance doesn't return MEMPOOL_ENTRY and DIFFERENT_WITNESS. It returns
* INVALID, with the errors txn-already-in-mempool and txn-same-nonwitness-data-in-mempool
* respectively. In those cases, the txid or wtxid may be in the mempool for a TX_CONFLICT.
*/
struct MempoolAcceptResult {
    /** Used to indicate the results of mempool validation. */
    enum class ResultType {
        VALID, //!> Fully validated, valid.
        INVALID, //!> Invalid.
        MEMPOOL_ENTRY, //!> Valid, transaction was already in the mempool.
        DIFFERENT_WITNESS, //!> Not validated. A same-txid-different-witness tx (see m_other_wtxid) already exists in the mempool and was not replaced.
    };
    /** Result type. Present in all MempoolAcceptResults. */
    const ResultType m_result_type;

    /** Contains information about why the transaction failed. */
    const TxValidationState m_state;

    /** Mempool transactions replaced by the tx. */
    const std::list<CTransactionRef> m_replaced_transactions;
    /** Virtual size as used by the mempool, calculated using serialized size and sigops. */
    const std::optional<int64_t> m_vsize;
    /** Raw base fees in satoshis. */
    const std::optional<CAmount> m_base_fees;
    /** The feerate at which this transaction was considered. This includes any fee delta added
     * using prioritisetransaction (i.e. modified fees). If this transaction was submitted as a
     * package, this is the package feerate, which may also include its descendants and/or
     * ancestors (see m_wtxids_fee_calculations below).
     */
    const std::optional<CFeeRate> m_effective_feerate;
    /** Contains the wtxids of the transactions used for fee-related checks. Includes this
     * transaction's wtxid and may include others if this transaction was validated as part of a
     * package. This is not necessarily equivalent to the list of transactions passed to
     * ProcessNewPackage().
     * Only present when m_result_type = ResultType::VALID. */
    const std::optional<std::vector<Wtxid>> m_wtxids_fee_calculations;

    /** The wtxid of the transaction in the mempool which has the same txid but different witness. */
    const std::optional<Wtxid> m_other_wtxid;

    static MempoolAcceptResult Failure(TxValidationState state) {
        return MempoolAcceptResult(state);
    }

    static MempoolAcceptResult FeeFailure(TxValidationState state,
                                          CFeeRate effective_feerate,
                                          const std::vector<Wtxid>& wtxids_fee_calculations) {
        return MempoolAcceptResult(state, effective_feerate, wtxids_fee_calculations);
    }

    static MempoolAcceptResult Success(std::list<CTransactionRef>&& replaced_txns,
                                       int64_t vsize,
                                       CAmount fees,
                                       CFeeRate effective_feerate,
                                       const std::vector<Wtxid>& wtxids_fee_calculations) {
        return MempoolAcceptResult(std::move(replaced_txns), vsize, fees,
                                   effective_feerate, wtxids_fee_calculations);
    }

    static MempoolAcceptResult MempoolTx(int64_t vsize, CAmount fees) {
        return MempoolAcceptResult(vsize, fees);
    }

    static MempoolAcceptResult MempoolTxDifferentWitness(const Wtxid& other_wtxid) {
        return MempoolAcceptResult(other_wtxid);
    }

// Private constructors. Use static methods MempoolAcceptResult::Success, etc. to construct.
private:
    /** Constructor for failure case */
    explicit MempoolAcceptResult(TxValidationState state)
        : m_result_type(ResultType::INVALID), m_state(state) {
            Assume(!state.IsValid()); // Can be invalid or error
        }

    /** Constructor for success case */
    explicit MempoolAcceptResult(std::list<CTransactionRef>&& replaced_txns,
                                 int64_t vsize,
                                 CAmount fees,
                                 CFeeRate effective_feerate,
                                 const std::vector<Wtxid>& wtxids_fee_calculations)
        : m_result_type(ResultType::VALID),
        m_replaced_transactions(std::move(replaced_txns)),
        m_vsize{vsize},
        m_base_fees(fees),
        m_effective_feerate(effective_feerate),
        m_wtxids_fee_calculations(wtxids_fee_calculations) {}

    /** Constructor for fee-related failure case */
    explicit MempoolAcceptResult(TxValidationState state,
                                 CFeeRate effective_feerate,
                                 const std::vector<Wtxid>& wtxids_fee_calculations)
        : m_result_type(ResultType::INVALID),
        m_state(state),
        m_effective_feerate(effective_feerate),
        m_wtxids_fee_calculations(wtxids_fee_calculations) {}

    /** Constructor for already-in-mempool case. It wouldn't replace any transactions. */
    explicit MempoolAcceptResult(int64_t vsize, CAmount fees)
        : m_result_type(ResultType::MEMPOOL_ENTRY), m_vsize{vsize}, m_base_fees(fees) {}

    /** Constructor for witness-swapped case. */
    explicit MempoolAcceptResult(const Wtxid& other_wtxid)
        : m_result_type(ResultType::DIFFERENT_WITNESS), m_other_wtxid(other_wtxid) {}
};

/**
* Validation result for package mempool acceptance.
*/
struct PackageMempoolAcceptResult
{
    PackageValidationState m_state;
    /**
    * Map from wtxid to finished MempoolAcceptResults. The client is responsible
    * for keeping track of the transaction objects themselves. If a result is not
    * present, it means validation was unfinished for that transaction. If there
    * was a package-wide error (see result in m_state), m_tx_results will be empty.
    */
    std::map<Wtxid, MempoolAcceptResult> m_tx_results;

    explicit PackageMempoolAcceptResult(PackageValidationState state,
                                        std::map<Wtxid, MempoolAcceptResult>&& results)
        : m_state{state}, m_tx_results(std::move(results)) {}

    /** Constructor to create a PackageMempoolAcceptResult from a single MempoolAcceptResult */
    explicit PackageMempoolAcceptResult(const Wtxid& wtxid, const MempoolAcceptResult& result)
        : m_tx_results{ {wtxid, result} } {}
};

/**
 * Try to add a transaction to the mempool. This is an internal function and is exposed only for testing.
 * Client code should use ChainstateManager::ProcessTransaction()
 *
 * @param[in]  active_chainstate  Reference to the active chainstate.
 * @param[in]  tx                 The transaction to submit for mempool acceptance.
 * @param[in]  accept_time        The timestamp for adding the transaction to the mempool.
 *                                It is also used to determine when the entry expires.
 * @param[in]  bypass_limits      When true, don't enforce mempool fee and capacity limits,
 *                                and set entry_sequence to zero.
 * @param[in]  test_accept        When true, run validation checks but don't submit to mempool.
 *
 * @returns a MempoolAcceptResult indicating whether the transaction was accepted/rejected with reason.
 */
MempoolAcceptResult AcceptToMemoryPool(Chainstate& active_chainstate, const CTransactionRef& tx,
                                       int64_t accept_time, bool bypass_limits, bool test_accept)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
 * AcceptToMemoryPool() for a transaction from outside validation: a peer's, an RPC's, the wallet's
 * (ChainstateManager::ProcessTransaction()) or one of the mempool file (LoadMempool()). Unless it is only
 * tested, the signals of the blocks connected so far are handed over first (precondition O7), so that it is
 * announced after the blocks it builds on. A reorganisation's re-adds call AcceptToMemoryPool() directly,
 * before the new blocks' signals.
 */
MempoolAcceptResult SubmitToMemoryPool(Chainstate& active_chainstate, const CTransactionRef& tx,
                                       int64_t accept_time, bool bypass_limits, bool test_accept)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
* Validate (and maybe submit) a package to the mempool. See doc/policy/packages.md for full details
* on package validation rules.
* @param[in]    test_accept         When true, run validation checks but don't submit to mempool.
* @param[in]    client_maxfeerate    If exceeded by an individual transaction, rest of (sub)package evaluation is aborted.
*                                   Only for sanity checks against local submission of transactions.
* @returns a PackageMempoolAcceptResult which includes a MempoolAcceptResult for each transaction.
* If a transaction fails, validation will exit early and some results may be missing. It is also
* possible for the package to be partially submitted.
*/
PackageMempoolAcceptResult ProcessNewPackage(Chainstate& active_chainstate, CTxMemPool& pool,
                                                   const Package& txns, bool test_accept, const std::optional<CFeeRate>& client_maxfeerate)
                                                   EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/* Mempool validation helper functions */

/**
 * Check if transaction will be final in the next block to be created.
 */
bool CheckFinalTxAtTip(const CBlockIndex& active_chain_tip, const CTransaction& tx) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/**
 * Calculate LockPoints required to check if transaction will be BIP68 final in the next block
 * to be created on top of tip.
 *
 * @param[in]   tip             Chain tip for which tx sequence locks are calculated. For
 *                              example, the tip of the current active chain.
 * @param[in]   coins_view      Any CCoinsView that provides access to the relevant coins for
 *                              checking sequence locks. For example, it can be a CCoinsViewCache
 *                              that isn't connected to anything but contains all the relevant
 *                              coins, or a CCoinsViewMemPool that is connected to the
 *                              mempool and chainstate UTXO set. In the latter case, the caller
 *                              is responsible for holding the appropriate locks to ensure that
 *                              calls to GetCoin() return correct coins.
 * @param[in]   tx              The transaction being evaluated.
 *
 * @returns The resulting height and time calculated and the hash of the block needed for
 *          calculation, or std::nullopt if there is an error.
 */
std::optional<LockPoints> CalculateLockPointsAtTip(
    CBlockIndex* tip,
    const CCoinsView& coins_view,
    const CTransaction& tx);

/**
 * Check if transaction will be BIP68 final in the next block to be created on top of tip.
 * @param[in]   tip             Chain tip to check tx sequence locks against. For example,
 *                              the tip of the current active chain.
 * @param[in]   lock_points     LockPoints containing the height and time at which this
 *                              transaction is final.
 * Simulates calling SequenceLocks() with data from the tip passed in.
 * The LockPoints should not be considered valid if CheckSequenceLocksAtTip returns false.
 */
bool CheckSequenceLocksAtTip(CBlockIndex* tip,
                             const LockPoints& lock_points);

/**
 * Closure representing one script verification
 * Note that this stores references to the spending transaction
 */
class CScriptCheck
{
private:
    CTxOut m_tx_out;
    const CTransaction *ptxTo;
    unsigned int nIn;
    script_verify_flags m_flags;
    bool cacheStore;
    PrecomputedTransactionData *txdata;
    SignatureCache* m_signature_cache;

public:
    CScriptCheck(const CTxOut& outIn, const CTransaction& txToIn, SignatureCache& signature_cache, unsigned int nInIn, script_verify_flags flags, bool cacheIn, PrecomputedTransactionData* txdataIn) :
        m_tx_out(outIn), ptxTo(&txToIn), nIn(nInIn), m_flags(flags), cacheStore(cacheIn), txdata(txdataIn), m_signature_cache(&signature_cache) { }

    CScriptCheck(const CScriptCheck&) = delete;
    CScriptCheck& operator=(const CScriptCheck&) = delete;
    CScriptCheck(CScriptCheck&&) = default;
    CScriptCheck& operator=(CScriptCheck&&) = default;

    std::optional<std::pair<ScriptError, std::string>> operator()();
};

// CScriptCheck is used a lot in std::vector, make sure that's efficient
static_assert(std::is_nothrow_move_assignable_v<CScriptCheck>);
static_assert(std::is_nothrow_move_constructible_v<CScriptCheck>);
static_assert(std::is_nothrow_destructible_v<CScriptCheck>);

/**
 * Convenience class for initializing and passing the script execution cache
 * and signature cache.
 */
class ValidationCache
{
private:
    //! Pre-initialized hasher to avoid having to recreate it for every hash calculation.
    CSHA256 m_script_execution_cache_hasher;

    /**
     * Guards m_script_execution_cache. CuckooCache tolerates concurrent lookups
     * and erasures but no insert concurrent with anything, so lookups take the
     * lock shared and inserts take it exclusively: block connection consults
     * the cache without cs_main while mempool acceptance inserts into it.
     */
    mutable std::shared_mutex m_script_execution_cache_mutex;
    CuckooCache::cache<uint256, SignatureCacheHasher> m_script_execution_cache;

public:
    SignatureCache m_signature_cache;

    ValidationCache(size_t script_execution_cache_bytes, size_t signature_cache_bytes);

    ValidationCache(const ValidationCache&) = delete;
    ValidationCache& operator=(const ValidationCache&) = delete;

    //! Return a copy of the pre-initialized hasher.
    CSHA256 ScriptExecutionCacheHasher() const { return m_script_execution_cache_hasher; }

    /**
     * Whether the script execution identified by `entry` was already verified.
     * With `erase`, a hit is marked for garbage collection: a block connection
     * is the last time an entry made by mempool acceptance is useful.
     */
    bool ScriptExecutionCached(const uint256& entry, bool erase) const
    {
        std::shared_lock lock{m_script_execution_cache_mutex};
        return m_script_execution_cache.contains(entry, erase);
    }

    void CacheScriptExecution(const uint256& entry)
    {
        std::unique_lock lock{m_script_execution_cache_mutex};
        m_script_execution_cache.insert(entry);
    }

    void TestOnlyResetScriptExecutionCache()
    {
        std::unique_lock lock{m_script_execution_cache_mutex};
        m_script_execution_cache.TestOnlyReset();
    }
};

/** Functions for validating blocks and updating the block tree */

/** Context-independent validity checks */
bool CheckBlock(const CBlock& block, BlockValidationState& state, const Consensus::Params& consensusParams, bool fCheckPOW = true, bool fCheckMerkleRoot = true);

/**
 * Verify a block, including transactions.
 *
 * @param[in]   block       The block we want to process. Must connect to the
 *                          current tip.
 * @param[in]   chainstate  The chainstate to connect to.
 * @param[in]   check_pow   perform proof-of-work check, nBits in the header
 *                          is always checked
 * @param[in]   check_merkle_root check the merkle root
 *
 * @return Valid or Invalid state. This doesn't currently return an Error state,
 *         and shouldn't unless there is something wrong with the existing
 *         chainstate. (This is different from functions like AcceptBlock which
 *         can fail trying to save new data.)
 *
 * For signets the challenge verification is skipped when check_pow is false.
 */
BlockValidationState TestBlockValidity(
    Chainstate& chainstate,
    const CBlock& block,
    bool check_pow,
    bool check_merkle_root) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/** Check that the proof of work on each blockheader matches the value in nBits */
bool HasValidProofOfWork(std::span<const CBlockHeader> headers, const Consensus::Params& consensusParams);

/** Check if a block has been mutated (with respect to its merkle root and witness commitments). */
bool IsBlockMutated(const CBlock& block, bool check_witness_root);

/** Return the sum of the claimed work on a given set of headers. No verification of PoW is done. */
arith_uint256 CalculateClaimedHeadersWork(std::span<const CBlockHeader> headers);

enum class VerifyDBResult {
    SUCCESS,
    CORRUPTED_BLOCK_DB,
    INTERRUPTED,
    SKIPPED_L3_CHECKS,
    SKIPPED_MISSING_BLOCKS,
};

/** RAII wrapper for VerifyDB: Verify consistency of the block and coin databases */
class CVerifyDB
{
private:
    kernel::Notifications& m_notifications;

public:
    explicit CVerifyDB(kernel::Notifications& notifications);
    ~CVerifyDB();
    [[nodiscard]] VerifyDBResult VerifyDB(
        Chainstate& chainstate,
        const Consensus::Params& consensus_params,
        CCoinsView& coinsview,
        int nCheckLevel,
        int nCheckDepth) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
};

enum DisconnectResult
{
    DISCONNECT_OK,      // All good.
    DISCONNECT_UNCLEAN, // Rolled back, but UTXO set was inconsistent with block.
    DISCONNECT_FAILED   // Something else went wrong.
};

/**
 * Which consensus deployments are active for a block. Evaluated by the node
 * from the header chain (ChainstateManager::MakeBlockValidationContext()), so
 * that the validation functions need neither the ChainstateManager nor the
 * version bits cache.
 */
struct DeploymentFlags {
    bool heightincb{false}; //!< BIP34
    bool dersig{false};     //!< BIP66
    bool cltv{false};       //!< BIP65
    bool csv{false};        //!< BIP68, BIP112, BIP113
    bool segwit{false};     //!< BIP141, BIP143, BIP147
};

/**
 * Ordering preconditions.
 *
 * Guarantees the rest of the node relies on that no type or annotation
 * states. The code that relies on one names its number; where the condition
 * can be tested it is asserted (Assume). Each entry says what holds it: the
 * construction, a named call that waits until it holds, or a deferral. A
 * call site that relies on a condition calls the wait; none reads the state
 * it waits for by itself.
 *
 *  O1  The only half-processed state a block can be seen in is "stored, not
 *      yet activated", from ProcessNewBlock() until the connector has
 *      activated it. Wait: WaitForPendingBlocks(), after
 *      PeerManagerImpl::WaitForPendingBlocks(), before the handler of a
 *      message that is judged against the tip (node/message_tip_rule.h) and
 *      before an orphan is considered again. Not in initial block download,
 *      where no transaction is accepted: there ProcessGetBlockData() and
 *      the GETBLOCKS handler answer from what is connected, outside it they
 *      activate before they answer.
 *  O2  When AcceptBlock() returns for a new block, its data, its position
 *      and BLOCK_VALID_TRANSACTIONS are recorded together (asserted there),
 *      in one hold of cs_main after the write (FinishAcceptBlock()). Until
 *      then the block is being stored (IsBlockBeingStored()), which no
 *      reader of its data takes for stored; a second copy is dropped as a
 *      block already had (PrepareAcceptBlockHeader()), and submitblock
 *      answers "duplicate" for it.
 *  O3  BlockChecked is delivered under cs_main before the tip moves to the
 *      block (PublishConnectTip()).
 *  O4  NewPoWValidBlock is delivered under cs_main before the block is
 *      recorded as stored, and only for a block on top of the tip
 *      (AcceptBlock(), not asserted). Deferral: a peer's request for the
 *      announced block is asked again until the block is stored
 *      (ProcessGetBlockData()).
 *  O5  Message processing requests only blocks that are not stored. Not
 *      asserted: getblockfrompeer checks and requests in two holds of
 *      cs_main. A block is "received, not stored" inside the handling of
 *      the message that delivers it (O13) and while its task runs on the
 *      connect pool. Deferral: its request stays in flight until the block
 *      is stored, and a second copy from the same peer is dropped while the
 *      first one's task runs (HandOffBlock()). A peer that goes takes its
 *      requests with it, and the block may be downloaded a second time.
 *  O6  The tip is the fully connected frontier: its coins are in the tip
 *      view and its transaction counts are known. Asserted in PublishTip(),
 *      where after a failed write-out the tip view's best block may be the
 *      tip's parent (DisconnectTip()) or the block that was being connected
 *      (PublishConnectTip()). Window blocks are connected ahead of the tip,
 *      each on a view of its own.
 *  O7  Signals are queued under cs_main in the order of connection, and
 *      what they name is stored and connected (checked in
 *      PublishConnectTip() and DisconnectTip()). BlockDisconnected is
 *      queued directly, BlockConnected behind the block's undo write, so
 *      the callers of DisconnectTip() settle the latter before the first
 *      disconnect (PlanConnectStep(), InvalidateBlock()). Wait:
 *      BlockManager::SettleUndoWrites() under cs_main: outside initial
 *      block download before the step that connected the block releases
 *      cs_main, in it before a transaction is submitted to the mempool
 *      (SubmitToMemoryPool(), ProcessNewPackage()).
 *  O8  A flush never sees a half-applied block (asserted in
 *      FlushStateToDisk()): the coins' best block is the tip, its parent
 *      (inside DisconnectTip(), which writes before the tip moves), or the
 *      one block on top of the tip that is being connected, or was when a
 *      write-out failed. The write is made before the block becomes the
 *      tip (PublishConnectTip()), and nothing can fail once it is; a step
 *      that disconnected blocks writes at its end (FinishConnectStep()),
 *      where a failure is the same fatal error. Wait:
 *      Chainstate::SettleWriteOut() before another write-out run starts, a
 *      flush goes on or the block index is written.
 *  O9  Publication is strictly sequential: a block is published on top of
 *      the tip (asserted in PublishConnectTip()), one ActivateBestChain()
 *      at a time (m_chainstate_mutex).
 *  O10 nSequenceId is assigned under cs_main in the order in which blocks
 *      are recorded as stored (ReceivedBlockTransactions()), which may
 *      differ from the order of arrival.
 *  O11 A file position copied under cs_main stays readable afterwards, or
 *      the reader handles the failed read (node::BlockFacts). A connected
 *      block's undo position is null until its queued write has run. Wait:
 *      BlockManager::SettleUndoWriteOf() for a reader of one block's undo
 *      data, SettleUndoWrites() before blocks are disconnected or verified.
 *  O12 The block index invariants hold whenever cs_main is free
 *      (CheckBlockIndex()).
 *  O13 Messages are processed on one thread, so a block request may be
 *      removed before the block is stored and no message asks for the
 *      block again in between. Asserted at the BLOCK message and in
 *      ProcessCompactBlockTxns(); for a block on the connect pool see O5.
 *  O14 InvalidateBlock() tolerates blocks arriving between its lock drops,
 *      by going over the block index again at its end.
 *  O15 The tip does not move while a block template or proposal is
 *      checked: TestBlockValidity() runs under cs_main, and the tip is
 *      published under cs_main only.
 */

/**
 * Everything a block validation function needs from the node besides the
 * block itself and the coins view. The node builds one under cs_main from
 * configuration and the header chain; the validation functions read it and
 * touch no other node state, so they can be called and tested without a
 * ChainstateManager. The contextual header and block checks also take the
 * previous block index entry, for the difficulty calculation.
 *
 * Every member is a copy, with two exceptions: params refers to the chain's
 * constant parameters, and median_time_past_at is a function, because which
 * ancestors a block's relative lock times refer to is known only once its
 * inputs are looked up.
 */
struct BlockValidationContext {
    const Consensus::Params& params;
    //! Clock reading used for the future-timestamp check of the header.
    NodeClock::time_point now;
    DeploymentFlags deployments;
    //! Script flags to enforce for this block, see GetBlockScriptFlags().
    script_verify_flags script_flags;
    //! Whether to verify input scripts at all. False only under -assumevalid.
    bool check_scripts{true};
    //! The block's hash.
    uint256 block_hash;
    //! Hash of the previous block index entry; null for the genesis block. The
    //! coins view must be at this block.
    uint256 prev_block_hash;
    //! The block's height.
    int height{0};
    //! Median time past of the previous block; 0 for the genesis block.
    int64_t prev_median_time_past{0};
    //! Whether the block is checked for BIP30, see EnforceBIP30().
    bool enforce_bip30{true};
    /**
     * Median time past of the block's ancestor at a height below the block's
     * own (BIP68 measures a time lock from the block before the spent coin's).
     * The node's implementation reads only what the hold of cs_main that
     * creates or loads an index entry writes and nothing writes after, so it
     * may be called without cs_main for as long as the block index lives.
     */
    std::function<int64_t(int)> median_time_past_at;
    /**
     * Whether a transaction whose inputs are not all in the view is deferred rather than a consensus
     * failure. True only while blocks below this one are still being applied (the window is incomplete):
     * the transaction's outputs are created, its other checks wait for FinishDeferredTransactions() once
     * every lower block is in. With a complete window a missing input is "bad-txns-inputs-missingorspent".
     */
    bool defer_missing_inputs{false};
    //! Where the block is on disk, as the index recorded it when the block was planned, for a worker that
    //! reads the block itself. Null where the block is not read from there.
    FlatFilePos block_pos{};
    //! A floor of the block's size in memory where it is known without visiting the block (the block cache
    //! measured it), else zero. For the count of the memory in flight (util/byte_ledger.h).
    size_t known_bytes{0};
};

/** Outputs of the pure ConnectBlock() besides the mutated coins view.
 *  ConnectBlock() resets the object it is given before it fills it. */
struct ConnectBlockResult {
    //! Undo data for the connected block; of no meaning when connection failed.
    CBlockUndo undo;
    //! The serialized size of `undo`, when it was measured ahead of FinishConnectBlock() (a window block's
    //! worker does, so that the connector thread need not walk the undo data).
    std::optional<uint32_t> undo_size;
    int num_inputs{0};
    int64_t sigops_cost{0};
    //! True when the block failed CheckBlock() before any UTXO work was done.
    bool failed_in_check_block{false};
    //! Indices into block.vtx of the transactions deferred because an input was not in the view yet
    //! (BlockValidationContext::defer_missing_inputs); their undo slots are empty until
    //! FinishDeferredTransactions() fills them. The block is not valid until that has run.
    std::vector<uint32_t> deferred_txs;
    //! The fees of the transactions checked so far; completed by FinishDeferredTransactions().
    CAmount fees{0};
    //! True when deferred transactions kept the coinbase amount check from running.
    bool coinbase_check_deferred{false};
    //! Phase boundaries, for the caller's bench logging.
    SteadyClock::time_point time_start{};
    SteadyClock::time_point time_checked{};
    SteadyClock::time_point time_forks{};
    SteadyClock::time_point time_connected{};
    SteadyClock::time_point time_verified{};
};

/**
 * Apply `block` to `view`, checking every consensus rule that needs the UTXO
 * set. Pure apart from the view and from what it is handed to use: it reads
 * node state only through the context's callbacks (median_time_past_at),
 * reads and updates the script execution cache, erases the entries of the
 * signature cache that its script checks hit (as on master: a block's
 * signatures are not looked up twice), reads the steady clock for its
 * timings, writes nothing to disk and logs nothing itself (CheckBlock() does,
 * in a debug category, where a signet block's solution fails). Script checks run
 * on `check_queue` when it is
 * non-null and has worker threads, otherwise inline; they are skipped
 * entirely when `ctx.check_scripts` is false. With `just_check` the block's
 * PoW and merkle root are not rechecked, the script execution cache takes the
 * results of the checks that run inline, and the view's best block is left
 * untouched.
 *
 * Script checks run on `check_queue` when it is non-null and has worker
 * threads, otherwise inline; they are skipped when `ctx.check_scripts` is
 * false. With `just_check` the block's PoW and merkle root are not rechecked,
 * the script execution cache takes the results of the checks that run inline,
 * and the view's best block is left untouched.
 *
 * On failure `state` holds the reason and the view may be partially updated.
 * With `ctx.defer_missing_inputs` true is returned with the checks of the
 * deferred transactions and the block's totals outstanding, and the block is
 * valid only once FinishDeferredTransactions() has returned true as well.
 *
 * `block` may be shared with readers on other threads: the fields
 * CheckBlock() records its results in are atomic (BlockCheckedFlag).
 */
bool ConnectBlock(const CBlock& block, const BlockValidationContext& ctx,
                  CCoinsViewCache& view, CCheckQueue<CScriptCheck>* check_queue,
                  ValidationCache& validation_cache, bool just_check,
                  BlockValidationState& state, ConnectBlockResult& result);

/**
 * Finish a block that deferred transactions, once every lower block's first pass is applied. A lower
 * block's own continuation may still be pending; what it changes meanwhile is a store conflict, settled by
 * connecting the block again alone. Checks each deferred transaction in full (inputs present and unspent,
 * amounts, maturity, BIP68, sigops, scripts), spends its inputs into its undo slot, completes the fee total
 * and the sigop count and runs the coinbase amount check. Pure in the same sense as ConnectBlock(). On
 * failure `state` holds the reason; the view may be partially updated.
 */
bool FinishDeferredTransactions(const CBlock& block, const BlockValidationContext& ctx,
                                CCoinsViewCache& view, ValidationCache& validation_cache,
                                BlockValidationState& state, ConnectBlockResult& result);

struct ConnectStep;
struct StagedConnect;
struct WindowSync;
namespace connect_phase {
struct Validated;
} // namespace connect_phase

/**
 * Placed in a scope right after cs_main is locked, when the scope may move a
 * tip: publishes the tip (ChainstateManager::PublishTip()) as the last thing
 * before the lock is released, on every way out of the scope.
 *
 * Publishers nest, as holds of cs_main do: the outermost one publishes. A
 * hold that outlives its scope (a step of ActivateBestChain() that keeps
 * the lock across several scopes) carries a publisher of its own for as
 * long as it lasts, so that no scope inside it publishes a tip the hold
 * has not finished moving.
 */
class TipPublisher
{
    ChainstateManager& m_chainman;

public:
    explicit TipPublisher(ChainstateManager& chainman) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    ~TipPublisher();
    TipPublisher(const TipPublisher&) = delete;
    TipPublisher& operator=(const TipPublisher&) = delete;
};

/** @see Chainstate::FlushStateToDisk */
inline constexpr std::array FlushStateModeNames{"NONE", "IF_NEEDED", "PERIODIC", "FORCE_FLUSH", "FORCE_SYNC"};
enum class FlushStateMode: uint8_t {
    NONE,
    IF_NEEDED,
    PERIODIC,
    FORCE_FLUSH,
    FORCE_SYNC,
};

/**
 * A convenience class for constructing the CCoinsView* hierarchy used
 * to facilitate access to the UTXO set.
 *
 * This class consists of an arrangement of layered CCoinsView objects,
 * preferring to store and retrieve coins in memory via the young tier, `m_store`,
 * but ultimately falling back on misses to the canonical store of UTXOs on
 * disk, `m_dbview`.
 */
class CoinsViews {

public:
    //! The lowest level of the CoinsViews cache hierarchy sits in a leveldb database on disk.
    //! All unspent coins reside in this store.
    CCoinsViewDB m_dbview GUARDED_BY(cs_main);

    //! This view wraps access to the leveldb instance and handles read errors gracefully.
    CCoinsViewErrorCatcher m_catcherview GUARDED_BY(cs_main);

    //! This is the top layer of the cache hierarchy: the young tier of the UTXO set, holding every coin
    //! changed since it was last written out and as many clean coins as fit per the dbcache setting.
    std::unique_ptr<CoinsStore> m_store GUARDED_BY(cs_main);

    //! The views blocks are connected into, one per block in flight, each a BlockApplyView over m_store: a
    //! block's changes reach the store once it is validated, under the block's identity, and are confirmed
    //! or unwound by one status flip, so an invalid block never pollutes what readers see. The first is the
    //! tip's (prefetch on the connect pool, the check queue); the others are the window's, each applied by
    //! a connect pool worker.
    std::vector<std::unique_ptr<BlockApplyView>> m_apply_views GUARDED_BY(cs_main);
    BlockApplyView& TipView() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) { return *m_apply_views.front(); }
    //! A view with no block open, or nullptr when every view is in use.
    BlockApplyView* FreeApplyView() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! This constructor initializes CCoinsViewDB and CCoinsViewErrorCatcher instances, but it
    //! *does not* create a CCoinsViewCache instance by default. This is done separately because the
    //! presence of the cache has implications on whether or not we're allowed to flush the cache's
    //! state to disk, which should not be done until the health of the database is verified.
    //!
    //! All arguments forwarded onto CCoinsViewDB.
    CoinsViews(DBParams db_params, CoinsViewOptions options);

    //! Initialize the tier and `window` views over it, the connect pool shared by them.
    void InitCache(util::NotNullSharedPtr<ThreadPool> connect_pool, int window) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};

enum class CoinsCacheSizeState
{
    //! The coins cache is in immediate need of a flush.
    CRITICAL = 2,
    //! The cache is at >= 90% capacity.
    LARGE = 1,
    OK = 0
};

constexpr int64_t LargeCoinsCacheThreshold(int64_t total_space) noexcept
{
    // No periodic flush needed if at least this much space is free
    constexpr int64_t MAX_BLOCK_COINSDB_USAGE_BYTES{int64_t(10_MiB)};
    return std::max((total_space * 9) / 10,
                    total_space - MAX_BLOCK_COINSDB_USAGE_BYTES);
}

//! Chainstate assumeutxo validity.
enum class Assumeutxo {
    //! Every block in the chain has been validated.
    VALIDATED,
    //! Blocks after an assumeutxo snapshot have been validated but the snapshot itself has not been validated.
    UNVALIDATED,
    //! The assumeutxo snapshot failed validation.
    INVALID,
};

/**
 * Chainstate stores and provides an API to update our local knowledge of the
 * current best chain.
 *
 * Eventually, the API here is targeted at being exposed externally as a
 * consumable library, so any functions added must only call
 * other class member functions, pure functions in other parts of the consensus
 * library, callbacks via the validation interface, or read/write-to-disk
 * functions (eventually this will also be via callbacks).
 *
 * Anything that is contingent on the current tip of the chain is stored here,
 * whereas block information and metadata independent of the current tip is
 * kept in `BlockManager`.
 */
class Chainstate
{
protected:
    /**
     * The ChainState Mutex
     * A lock that must be held when modifying this ChainState - held in ActivateBestChain() and
     * InvalidateBlock()
     */
    Mutex m_chainstate_mutex;

    //! Optional mempool that is kept in sync with the chain.
    //! Only the active chainstate has a mempool.
    CTxMemPool* m_mempool;

    //! Manages the UTXO set, which is a reflection of the contents of `m_chain`.
    std::unique_ptr<CoinsViews> m_coins_views;

    //! Cached result of LookupBlockIndex(*m_from_snapshot_blockhash)
    mutable const CBlockIndex* m_cached_snapshot_base GUARDED_BY(::cs_main){nullptr};

    //! Target block for this chainstate. If this is not set, chainstate will
    //! target the most-work, valid block. If this is set, ChainstateManager
    //! considers this a "historical" chainstate since it will only contain old
    //! blocks up to the target block, not newer blocks.
    std::optional<uint256> m_target_blockhash GUARDED_BY(::cs_main);

    //! Cached result of LookupBlockIndex(*m_target_blockhash)
    mutable const CBlockIndex* m_cached_target_block GUARDED_BY(::cs_main){nullptr};

    std::optional<const char*> m_last_script_check_reason_logged GUARDED_BY(::cs_main){};

public:
    //! Reference to a BlockManager instance which itself is shared across all
    //! Chainstate instances.
    node::BlockManager& m_blockman;

    //! The chainstate manager that owns this chainstate. The reference is
    //! necessary so that this instance can check whether it is the active
    //! chainstate within deeply nested method calls.
    ChainstateManager& m_chainman;

    //! Set while a reorganisation's step adds the transactions of the disconnected blocks back to the mempool
    //! (FinishConnectStep()). Mempool acceptance then leaves the write-out check to the step: a flush between
    //! two of those transactions could settle the effects queue and hand on the signals of the step's
    //! connected blocks before the later transactions' (precondition O7).
    bool m_readding_disconnected GUARDED_BY(::cs_main){false};

    explicit Chainstate(
        CTxMemPool* mempool,
        node::BlockManager& blockman,
        ChainstateManager& chainman,
        std::optional<uint256> from_snapshot_blockhash = std::nullopt);

    //! Return path to chainstate leveldb directory.
    fs::path StoragePath() const;

    //! Return the current role of the chainstate. See `ChainstateManager`
    //! documentation for a description of the different types of chainstates.
    //!
    //! @sa ChainstateRole
    kernel::ChainstateRole GetRole() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Initialize the CoinsViews UTXO set database management data structures. The in-memory
     * cache is initialized separately.
     *
     * All parameters forwarded to CoinsViews.
     */
    void InitCoinsDB(
        size_t cache_size_bytes,
        bool in_memory,
        bool should_wipe);

    //! Initialize the in-memory coins cache (to be done after the health of the on-disk database
    //! is verified).
    void InitCoinsCache(size_t cache_size_bytes) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! @returns whether or not the CoinsViews object has been fully initialized and we can
    //!          safely flush this object to disk.
    bool CanFlushToDisk() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return m_coins_views && m_coins_views->m_store;
    }

    //! The current chain of blockheaders we consult and build on.
    //! @see CChain, CBlockIndex.
    CChain m_chain;

    //! Assumeutxo state indicating whether all blocks in the chain were
    //! validated, or if the chainstate is based on an assumeutxo snapshot and
    //! the snapshot has not been validated.
    Assumeutxo m_assumeutxo GUARDED_BY(::cs_main);

    /**
     * The blockhash which is the base of the snapshot this chainstate was created from.
     *
     * std::nullopt if this chainstate was not created from a snapshot.
     */
    const std::optional<uint256> m_from_snapshot_blockhash;

    //! Hash of the UTXO set at the target block, computed when the chainstate
    //! reaches the target block, and null before then.
    std::optional<AssumeutxoHash> m_target_utxohash GUARDED_BY(::cs_main);

    /**
     * The base of the snapshot this chainstate was created from.
     *
     * nullptr if this chainstate was not created from a snapshot.
     */
    const CBlockIndex* SnapshotBase() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Return target block which chainstate tip is expected to reach, if this
    //! is a historic chainstate being used to validate a snapshot, or null if
    //! chainstate targets the most-work block. Requires the block index to be
    //! loaded, so prefer TargetBlockHash() when the block itself is not needed.
    const CBlockIndex* TargetBlock() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Return hash of the target block, or nullopt if chainstate targets the
    //! most-work block. Unlike TargetBlock(), does not require the block index
    //! to be loaded.
    std::optional<uint256> TargetBlockHash() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return m_target_blockhash;
    }
    //! Set target block for this chainstate. If null, chainstate will target
    //! the most-work valid block. If non-null chainstate will be a historic
    //! chainstate and target the specified block.
    void SetTargetBlock(CBlockIndex* block) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Set target block for this chainstate using just a block hash. Useful
    //! when the block database has not been loaded yet.
    void SetTargetBlockHash(uint256 block_hash) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Return true if chainstate reached target block.
    bool ReachedTarget() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        const CBlockIndex* target_block{TargetBlock()};
        assert(!target_block || target_block->GetAncestor(m_chain.Height()) == m_chain.Tip());
        return target_block && target_block == m_chain.Tip();
    }

    /**
     * The set of all CBlockIndex entries that have as much work as our current
     * tip or more, and transaction data needed to be validated (with
     * BLOCK_VALID_TRANSACTIONS for each block and its parents back to the
     * genesis block or an assumeutxo snapshot block). Entries may be failed,
     * though, and pruning nodes may be missing the data for the block.
     */
    std::set<CBlockIndex*, node::CBlockIndexWorkComparator> setBlockIndexCandidates;

    //! @returns A reference to the in-memory cache of the UTXO set.
    CCoinsViewCache& CoinsTip() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        Assert(m_coins_views);
        return *Assert(m_coins_views->m_store);
    }

    //! @returns The same object as CoinsTip(), with the young tier's own interface (the lag write-out).
    CoinsStore& CoinsStoreTip() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        Assert(m_coins_views);
        return *Assert(m_coins_views->m_store);
    }

    //! @returns A reference to the on-disk UTXO set database.
    CCoinsViewDB& CoinsDB() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return Assert(m_coins_views)->m_dbview;
    }

    //! @returns A pointer to the mempool.
    CTxMemPool* GetMempool()
    {
        return m_mempool;
    }

    //! @returns A reference to a wrapped view of the in-memory UTXO set that
    //!     handles disk read errors gracefully.
    CCoinsViewErrorCatcher& CoinsErrorCatcher() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return Assert(m_coins_views)->m_catcherview;
    }

    //! Destructs all objects related to accessing the UTXO set. A write-out run in flight lands first.
    void ResetCoinsViews() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! The coins database cache this chainstate was opened with or last assigned; the database keeps the cache
    //! it was opened with (ResizeCoinsCaches()).
    size_t m_coinsdb_cache_size_bytes{0};

    //! The cache size of the in-memory coins view.
    size_t m_coinstip_cache_size_bytes{0};

    /**
     * Start a write-out run of the young tier: every change at or below `bound_height` goes to the coins
     * database on the block manager's executor, behind the undo writes queued so far and together with the
     * block-file sync and the block index write, so that the marker never names a tip whose data is not
     * durable. The run streams from the tier a chunk at a time, each chunk clean as it lands. The completion
     * (under cs_main) records the head the database now holds, evicts clean entries down to `evict_to`
     * bytes if given, and notifies ChainStateFlushed. `final` drains the tier, and is forced while the
     * written head is not on the active chain: after a disconnect only a whole write leaves the database
     * at a block the marker can name. Returns false on a fatal error.
     */
    //! `mode` and `for_prune` are the flush's that starts the run, for the utxocache:flush tracepoint the run
    //! fires when it lands.
    bool StartWriteOutRun(BlockValidationState& state, int bound_height, bool final, std::optional<size_t> evict_to = {},
                          FlushStateMode mode = FlushStateMode::NONE, bool for_prune = false) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Wait for a write-out run in flight and every effect queued before now (the undo writes, and the
    //! sync of an undo file that the last of them completed) and apply their completions. Nothing is
    //! queued meanwhile: effects are queued under cs_main, which the caller holds. Precondition O8 with
    //! blocks in flight: no run is half way when the caller goes on, and what a write of the block
    //! index is about to record is on disk before it.
    void SettleWriteOut() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Resize the CoinsViews caches dynamically and flush state to disk.
    //! @returns true unless an error occurred during the flush.
    bool ResizeCoinsCaches(size_t coinstip_size, size_t coinsdb_size)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Update the on-disk chain state.
     * The caches and indexes are flushed depending on the mode we're called with
     * if they're too large, if it's been a while since the last write,
     * or always and in all cases if we're in prune mode and are deleting files.
     *
     * If FlushStateMode::NONE is used, then FlushStateToDisk(...) won't do anything
     * besides checking if we need to prune.
     *
     * @returns true unless a system error occurred
     */
    bool FlushStateToDisk(
        BlockValidationState& state,
        FlushStateMode mode,
        int nManualPruneHeight = 0);

    //! Flush all changes to disk.
    void ForceFlushStateToDisk(bool wipe_cache = true);

    //! Prune blockfiles from the disk if necessary and then flush chainstate changes
    //! if we pruned.
    void PruneAndFlush();

    /**
     * Find the best known block, and make it the tip of the block chain. The
     * result is either failure or an activated best chain. pblock is either
     * nullptr or a pointer to a block that is already loaded (to avoid loading
     * it again from disk).
     *
     * ActivateBestChain is split into steps (see PlanConnectStep), each block
     * of a step passing through StageConnectTip, its validation and
     * PublishConnectTip, so that cs_main is held per block, or in the window
     * for the blocks that are ready together, rather than for an extended
     * period (a reorganisation holds it from its first disconnect to the end
     * of its step); the length of this call may be quite long during
     * reindexing or a substantial reorg.
     *
     * May not be called with cs_main held. May not be called in a
     * validationinterface callback.
     *
     * Note that if this is called while a snapshot chainstate is active, and if
     * it is called on a validated chainstate whose tip has reached the base
     * block of the snapshot, its execution will take *MINUTES* while it hashes
     * the UTXO set to verify the assumeutxo value the snapshot was activated
     * with. `cs_main` will be held during this time.
     *
     * @returns true unless a system error occurred
     */
    bool ActivateBestChain(
        BlockValidationState& state,
        std::shared_ptr<const CBlock> pblock = nullptr)
        EXCLUSIVE_LOCKS_REQUIRED(!m_chainstate_mutex)
        LOCKS_EXCLUDED(::cs_main);

    // Block (dis)connection on a given view:
    DisconnectResult DisconnectBlock(const CBlock& block, const CBlockIndex* pindex, CCoinsViewCache& view)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool ConnectBlock(const CBlock& block, BlockValidationState& state, CBlockIndex* pindex,
                      CCoinsViewCache& view, bool fJustCheck = false) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    // Apply the effects of a block disconnection on the UTXO set.
    bool DisconnectTip(BlockValidationState& state, DisconnectedBlockTransactions* disconnectpool) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_mempool->cs);

    // Manual block validity manipulation:
    /** Mark a block as precious and reorganize.
     *
     * May not be called in a validationinterface callback.
     */
    bool PreciousBlock(BlockValidationState& state, CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!m_chainstate_mutex)
        LOCKS_EXCLUDED(::cs_main);

    /** Mark a block as invalid. */
    bool InvalidateBlock(BlockValidationState& state, CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!m_chainstate_mutex)
        LOCKS_EXCLUDED(::cs_main);

    /** Set invalidity status to all descendants of a block */
    void SetBlockFailureFlags(CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Remove invalidity status from a block, its descendants and ancestors and reconsider them for activation */
    void ResetBlockFailureFlags(CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Replay blocks that aren't fully applied to the database. */
    bool ReplayBlocks();

    /** Whether the chain state needs to be redownloaded due to lack of witness data */
    [[nodiscard]] bool NeedsRedownload() const EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Add a block to the candidate set if it has as much work as the current tip. */
    void TryAddBlockIndexCandidate(CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    void PruneBlockIndexCandidates();

    void ClearBlockIndexCandidates() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Populate the candidate set by calling TryAddBlockIndexCandidate on all valid block indices. */
    void PopulateBlockIndexCandidates() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Find the last common block of this chain and a locator. */
    const CBlockIndex* FindForkInGlobalIndex(const CBlockLocator& locator) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Update the chain tip based on database information, i.e. CoinsTip()'s best block. */
    bool LoadChainTip() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Dictates whether we need to flush the cache to disk or not.
    //!
    //! @return the state of the size of the coins cache.
    CoinsCacheSizeState GetCoinsCacheSizeState() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    CoinsCacheSizeState GetCoinsCacheSizeState(
        size_t max_coins_cache_size_bytes,
        size_t max_mempool_size_bytes) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    std::string ToString() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Get the last block that was flushed to disk.
    const CBlockIndex* GetLastFlushedBlock() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main) { return m_last_flushed_block; }

    //! Indirection necessary to make lock annotations work with an optional mempool.
    RecursiveMutex* MempoolMutex() const LOCK_RETURNED(m_mempool->cs)
    {
        return m_mempool ? &m_mempool->cs : nullptr;
    }

    //! Return the [start, end] (inclusive) of block heights we can prune.
    //!
    //! start > end is possible, meaning no blocks can be pruned.
    std::pair<int, int> GetPruneRange(int last_height_can_prune) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

protected:
    /**
     * Begin a step towards step.target: disconnect the active blocks that are
     * not on its chain. The blocks to connect are then handed out in batches
     * by PlanNextBatch() and the step is closed by FinishConnectStep().
     * @returns true unless a system error occurred
     */
    bool PlanConnectStep(BlockValidationState& state, ConnectStep& step) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_mempool->cs);
    //! Place the next at most 32 blocks towards the target in step.batch. False when the target has been reached.
    bool PlanNextBatch(ConnectStep& step) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    //! Re-add disconnected transactions to the mempool and run the end-of-step checks. False, with `state`
    //! set, if the chainstate write the step's publications left out failed (a system error).
    [[nodiscard]] bool FinishConnectStep(BlockValidationState& state, ConnectStep& step) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_mempool->cs);

    /**
     * Connecting a block happens in three parts:
     *  - StageConnectTip(), under cs_main: open the block in a view of the
     *    store and decide the node policy (BlockValidationContext). At the
     *    tip the block is already read and the prefetch of the coins it
     *    spends is started; in the window the block takes a free view, defers
     *    the inputs lower blocks have not produced yet, and is read by its
     *    worker.
     *  - RunStagedConnect() at the tip: the pure ConnectBlock() into the view
     *    with the check queue, under cs_main. RunConnectTask() in the window:
     *    the same on a connect pool worker with its scripts inline and no lock
     *    held; RunContinuation() finishes what the block deferred.
     *  - PublishConnectTip(), under cs_main and the mempool lock: queue the
     *    undo write, confirm the block in the store, update mempool and tip,
     *    notify. Returns true unless a system error occurred, the block was
     *    invalid (state says which), or a window block is to be connected
     *    again alone (state valid).
     * What exists of the block at each part is a type of its own
     * (connect_phase, in validation.cpp); only a Validated is published.
     */
    void StageConnectTip(CBlockIndex* pindexNew, StagedConnect& staged, const FlatFilePos& pos, std::shared_ptr<const CBlock> block, size_t block_bytes) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    void RunStagedConnect(StagedConnect& staged) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    void RunConnectTask(StagedConnect& staged) noexcept;
    //! A windowed block's worker has applied it: mark it so and run the continuations this makes possible.
    void OnWindowBlockApplied(StagedConnect& staged);
    //! Finish a window block's deferred transactions and apply the result; the block is marked finished.
    void RunContinuation(StagedConnect& s, WindowSync& sync) noexcept;
    bool PublishConnectTip(
        BlockValidationState& state,
        StagedConnect& staged,
        connect_phase::Validated& validated,
        const kernel::ChainstateRole& role,
        DisconnectedBlockTransactions& disconnectpool,
        bool reorg) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_mempool->cs);
    //! Wait for the window's workers and take back every block still in flight, highest first.
    void DrainWindow(ConnectStep& step) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /**
     * The tip moved from `pindexFork` to `pindexNewTip`: UpdatedBlockTip through the effects executor
     * (behind the blocks' BlockConnected signals) and the kernel's blockTip notification. Returns true when
     * the latter asked to stop. Nothing is sent for a chainstate that is not the active one or when the two
     * blocks are the same.
     */
    bool NotifyTipAdvanced(CBlockIndex* pindexNewTip, const CBlockIndex* pindexFork, bool still_in_ibd) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * The assumevalid decision for a block: nullptr when its scripts are not
     * to be verified, otherwise the reason they are (logged once per change).
     */
    const char* ScriptCheckReason(const CBlockIndex& pindex) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /**
     * The node side of ConnectBlock() after the pure core ran: fatal error on a
     * mutated block, counters and bench logging, queueing the undo write and
     * the validity raise (unless just_check), the tracepoint. Returns the
     * final verdict. The undo data is moved out of result.
     */
    bool FinishConnectBlock(const CBlock& block, BlockValidationState& state, CBlockIndex* pindex, const char* script_check_reason, bool ok, ConnectBlockResult& result, bool just_check) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    void InvalidBlockFound(CBlockIndex* pindex, const BlockValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    CBlockIndex* FindMostWorkChain() EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /**
     * Block-index transition: the candidate chain ending in `top` cannot be
     * connected because `unusable` (an ancestor of `top` not on the active
     * chain) is invalid or lacks data. Removes the chain from the candidates,
     * records a new best invalid, and files the blocks that only lack data
     * under m_blocks_unlinked so their arrival makes them candidates again.
     */
    void DropUnusableCandidateChain(CBlockIndex* top, CBlockIndex* unusable, bool failed, bool missing_data) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    bool RollforwardBlock(const CBlockIndex* pindex, CCoinsViewCache& inputs) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    void CheckForkWarningConditions() EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    void InvalidChainFound(CBlockIndex* pindexNew) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Make mempool consistent after a reorg, by re-adding or recursively erasing
     * disconnected block transactions from the mempool, and also removing any
     * other transactions from the mempool that are no longer valid given the new
     * tip/height.
     *
     * Note: we assume that disconnectpool only contains transactions that are NOT
     * confirmed in the current chain nor already in the mempool (otherwise,
     * in-mempool descendants of such transactions would be removed).
     *
     * Passing fAddToMempool=false will skip trying to add the transactions back,
     * and instead just erase from the mempool as needed.
     */
    void MaybeUpdateMempoolForReorg(
        DisconnectedBlockTransactions& disconnectpool,
        bool fAddToMempool) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_mempool->cs);

    /** Check warning conditions and do some notifications on new chain tip set. */
    void UpdateTip(const CBlockIndex* pindexNew)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    NodeClock::time_point m_next_write{NodeClock::time_point::max()};
    const CBlockIndex* m_last_flushed_block GUARDED_BY(::cs_main){nullptr};
    //! Set by the connect window around its periodic write: a write-out run started there is waited for
    //! by the connector once it has released cs_main, not inside FlushStateToDisk(). So is the run that
    //! automatic pruning needs; the files are then chosen by the next flush.
    bool m_defer_write_out_wait GUARDED_BY(::cs_main){false};
    //! Set by the connect window around a block's publication: the write-out before the tip moves leaves
    //! automatic pruning to the periodic write of the same hold.
    bool m_prune_at_periodic_write GUARDED_BY(::cs_main){false};
    //! The write-out runs that failed (a fatal error is raised for each). What rests on a run having
    //! landed, the removal of pruned files, compares it across the wait.
    uint64_t m_write_out_runs_failed GUARDED_BY(::cs_main){0};
    //! A block failed in the connect window: the next step connects it alone, which decides whether it is
    //! invalid (PublishConnectTip()).
    bool m_window_retry_serial GUARDED_BY(::cs_main){false};

    /**
     * In case of an invalid snapshot, rename the coins leveldb directory so
     * that it can be examined for issue diagnosis.
     */
    [[nodiscard]] util::Result<void> InvalidateCoinsDBOnDisk() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    friend ChainstateManager;
};

enum class SnapshotCompletionResult {
    SUCCESS,
    SKIPPED,

    // Expected assumeutxo configuration data is not found for the height of the
    // base block.
    MISSING_CHAINPARAMS,

    // Failed to generate UTXO statistics (to check UTXO set hash) for the
    // validated chainstate.
    STATS_FAILED,

    // The UTXO set hash of the validated chainstate does not match the one
    // expected by assumeutxo chainparams.
    HASH_MISMATCH,
};

/**
 * Interface for managing multiple \ref Chainstate objects, where each
 * chainstate is associated with chainstate* subdirectory in the data directory
 * and contains a database of UTXOs existing at a different point in history.
 * (See \ref Chainstate class for more information.)
 *
 * Normally there is exactly one Chainstate, which contains the UTXO set of
 * chain tip if syncing is completed, or the UTXO set the most recent validated
 * block if the initial sync is still in progress.
 *
 * However, if an assumeutxo snapshot is loaded before syncing is completed,
 * there will be two chainstates. The original fully validated chainstate will
 * continue to exist and download new blocks in the background. But the new
 * snapshot which is loaded will become a second chainstate. The second
 * chainstate will be used as the chain tip for the wallet and RPCs even though
 * it is only assumed to be valid. When the initial chainstate catches up to the
 * snapshot height and confirms that the assumeutxo snapshot is actually valid,
 * the second chainstate will be marked validated and become the only chainstate
 * again.
 */
class ChainstateManager
{
private:

    /** The last header for which a headerTip notification was issued. */
    CBlockIndex* m_last_notified_header GUARDED_BY(GetMutex()){nullptr};

    node::PublishedTip m_published_tip;
    //! Counts the tip changes of every chainstate, and the publications that
    //! changed nothing but the initial block download state. The number a
    //! published tip carries.
    uint64_t m_tip_sequence GUARDED_BY(::cs_main){0};
    //! A cs_main hold has moved the tip of a chainstate, or changed which
    //! chainstate is the current one, and has not published yet.
    std::atomic_bool m_tip_unpublished{false};
    //! The publishers alive (TipPublisher): the last one to go publishes.
    int m_tip_publishers GUARDED_BY(::cs_main){0};
    friend class TipPublisher;
    //! Builds the value for the current chainstate's tip as it is now.
    node::TipSnapshot CurrentTipSnapshot() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    bool NotifyHeaderTip() LOCKS_EXCLUDED(GetMutex());

    //! Internal helper for ActivateSnapshot().
    //!
    //! De-serialization of a snapshot that is created with
    //! the dumptxoutset RPC.
    //! To reduce space the serialization format of the snapshot avoids
    //! duplication of tx hashes. The code takes advantage of the guarantee by
    //! leveldb that keys are lexicographically sorted.
    [[nodiscard]] util::Result<void> PopulateAndValidateSnapshot(
        Chainstate& snapshot_chainstate,
        AutoFile& coins_file,
        const node::SnapshotMetadata& metadata);

    /**
     * If a block header hasn't already been seen, call CheckBlockHeader on it, ensure
     * that it doesn't descend from an invalid block, and then add it to m_block_index.
     * Caller must set min_pow_checked=true in order to add a new header to the
     * block index (permanent memory storage), indicating that the header is
     * known to be part of a sufficiently high-work chain (anti-dos check).
     */
    bool AcceptBlockHeader(
        const CBlockHeader& block,
        BlockValidationState& state,
        CBlockIndex** ppindex,
        bool min_pow_checked) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    friend Chainstate;

    /** Most recent headers presync progress update, for rate-limiting. */
    MockableSteadyClock::time_point m_last_presync_update GUARDED_BY(GetMutex()){};

    //! A queue for script verifications that have to be performed by worker threads.
    CCheckQueue<CScriptCheck> m_script_check_queue;

    //! Blocks whose data is being written to disk with cs_main released
    //! (see PreparedBlock). A second arrival of such a block is treated
    //! as already stored, so it is written once.
    std::set<const CBlockIndex*> m_blocks_being_stored GUARDED_BY(::cs_main);

public:
    //! Whether another thread is writing the block to disk right now (it is indexed once the write is done).
    bool IsBlockBeingStored(const CBlockIndex& index) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main) { return m_blocks_being_stored.contains(&index); }
    //! The memory blocks hold from their arrival until nothing of them is left to do, by holder
    //! (util/byte_ledger.h). Declared before its holders.
    util::ByteLedger m_bytes_in_flight;
    //! Accepted blocks kept for the connect loop, so that one that arrived before its parent is connected from
    //! the object received and checked rather than read from disk and deserialized again. Filled by
    //! ProcessNewBlock() and the external block file loader, emptied by ActivateBestChain().
    node::BlockCache m_block_cache;

private:
    //! The window's: blocks validated on a connect pool worker, transactions deferred to their block's
    //! continuation because a lower block had not produced an input yet, the conflicts a store change met
    //! (a spend or create a lower block took, a create BIP30 refused, a miss), and the blocks that failed in
    //! the window and were connected again alone. Written by the connector thread and the pool's workers
    //! (deferred transactions are counted where a block is applied), read at shutdown.
    std::atomic<uint64_t> m_window_blocks{0};
    std::atomic<uint64_t> m_window_deferred_txs{0};
    std::atomic<uint64_t> m_window_conflicts{0};
    std::atomic<uint64_t> m_window_retries{0};

    //! The connector thread (see SignalConnector()). Where each block handed to it stands is one machine
    //! (node/handoff_machine.h); a waiter keeps the ticket of a block and waits for a state of it.
    std::thread m_connector_thread;
    mutable Mutex m_connector_mutex;
    std::condition_variable m_connector_cv;
    node::ConnectorHandOffs m_connector_handoffs GUARDED_BY(m_connector_mutex);
    //! Whether the connector takes blocks (util/lifecycle.h): Running from construction, with its thread
    //! started by the first block; Draining while StopConnector() waits for the pass that is running;
    //! Stopped for good after.
    util::Lifecycle m_connector_life GUARDED_BY(m_connector_mutex){util::LifeState::Running};
    //! The block most recently accepted for the connector, for its next activation (SignalConnector()).
    std::shared_ptr<const CBlock> m_connector_block GUARDED_BY(m_connector_mutex);
    void ConnectorLoop() EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex);

    //! Timers and counters used for benchmarking validation in both background
    //! and active chainstates.
    SteadyClock::duration GUARDED_BY(::cs_main) time_check{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_forks{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_connect{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_verify{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_undo{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_index{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_total{};
    int64_t GUARDED_BY(::cs_main) num_blocks_total{0};
    SteadyClock::duration GUARDED_BY(::cs_main) time_connect_total{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_flush{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_chainstate{};
    SteadyClock::duration GUARDED_BY(::cs_main) time_post_connect{};

protected:
    CBlockIndex* m_best_invalid GUARDED_BY(::cs_main){nullptr};

public:
    using Options = kernel::ChainstateManagerOpts;

    explicit ChainstateManager(const util::SignalInterrupt& interrupt, Options options, node::BlockManager::Options blockman_options);

    //! Function to restart active indexes; set dynamically to avoid a circular
    //! dependency on `base/index.cpp`.
    std::function<void()> snapshot_download_completed = std::function<void()>();

    const CChainParams& GetParams() const { return m_options.chainparams; }
    const Consensus::Params& GetConsensus() const { return m_options.chainparams.GetConsensus(); }
    bool ShouldCheckBlockIndex() const;
    const arith_uint256& MinimumChainWork() const { return *Assert(m_options.minimum_chain_work); }
    const uint256& AssumedValidBlock() const { return *Assert(m_options.assumed_valid_block); }
    kernel::Notifications& GetNotifications() const { return m_options.notifications; };

    /**
     * Build the context for validating the block `block_hash` whose parent is
     * `prev` (nullptr for genesis): consensus parameters, the current time,
     * the deployments active at that height and the script flags they imply.
     * `check_scripts` is left at true; callers that skip script verification
     * under -assumevalid clear it themselves.
     */
    BlockValidationContext MakeBlockValidationContext(const uint256& block_hash, const CBlockIndex* prev) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Make various assertions about the state of the block index.
     *
     * By default this only executes fully when using the Regtest chain; see: m_options.check_block_index.
     */
    void CheckBlockIndex() const;

    /**
     * Alias for ::cs_main.
     * Should be used in new code to make it easier to make ::cs_main a member
     * of this class.
     * Generally, methods of this class should be annotated to require this
     * mutex. This will make calling code more verbose, but also help to:
     * - Clarify that the method will acquire a mutex that heavily affects
     *   overall performance.
     * - Force call sites to think how long they need to acquire the mutex to
     *   get consistent results.
     */
    RecursiveMutex& GetMutex() const LOCK_RETURNED(::cs_main) { return ::cs_main; }

    const util::SignalInterrupt& m_interrupt;
    const Options m_options;
    //! A single BlockManager instance is shared across each constructed
    //! chainstate to avoid duplicating block metadata.
    node::BlockManager m_blockman;
    //! What the validation interface's pre-sync hook reaches (AddPreSyncHook()): this manager's block
    //! manager, disarmed when the manager goes, which may be before the signals or after them.
    std::shared_ptr<util::HookTarget<node::BlockManager>> m_pre_sync_target;

    /** Determine the potential consensus script flags for blocks in a chain */
    static script_verify_flags GetAllConsensusScriptFlags(const Consensus::Params& params);

    /** Store the potential consensus script flags for this manager's selected params */
    const script_verify_flags m_all_consensus_script_flags;

    ValidationCache m_validation_cache;

    /**
     * Whether initial block download (IBD) is ongoing.
     *
     * This value is used for lock-free IBD checks, and latches from true to
     * false once block loading has finished and the current chain tip has
     * enough work and is recent.
     */
    std::atomic_bool m_cached_is_ibd{true};

    /**
     * The tip of the current chainstate as one value, see node::TipSnapshot.
     * For a reader that needs one fact about the tip. The value is what the
     * reader would find under cs_main: while no hold of cs_main that moved
     * the tip is open, it is a copy of the published value and cs_main is
     * not taken; while one is open, the call takes cs_main and reads the
     * chain. A caller that already holds cs_main inside such a hold
     * therefore sees the tip of that moment.
     */
    node::TipSnapshot PublishedTip() const
    {
        if (m_tip_unpublished.load()) {
            LOCK(::cs_main);
            return CurrentTipSnapshot();
        }
        return m_published_tip.Get();
    }

    /**
     * Publish the current chainstate's tip. Called at the end of a cs_main
     * hold that moved a tip (TipPublisher), and directly wherever the choice
     * of the current chainstate or the initial block download latch (after
     * UpdateIBDStatus()) may have changed outside such a hold; does nothing
     * if nothing changed. The tip being published is the fully connected
     * frontier (precondition O6).
     */
    void PublishTip() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Record that a tip is about to move or the current chainstate is
     * about to change: the tip sequence number rises, and readers of
     * PublishedTip() take cs_main until the next PublishTip(). Called
     * before the change, so that no reader sees part of it without waiting
     * for the hold; UpdateIBDStatus() sets the same flag before it ends
     * initial block download. If a hold forgets to publish, readers keep
     * taking cs_main, which is slower and never wrong.
     */
    void MarkTipChanged() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        ++m_tip_sequence;
        m_tip_unpublished.store(true);
    }
    bool TipUnpublished() const { return m_tip_unpublished.load(); }

    /**
     * Every received block is assigned a unique and increasing identifier, so we
     * know which one to give priority in case of a fork.
     */
    /** Blocks loaded from disk are assigned id SEQ_ID_INIT_FROM_DISK{1}
     * (SEQ_ID_BEST_CHAIN_FROM_DISK{0} if they belong to the best chain loaded from disk),
     * so start the counter after that. **/
    int32_t nBlockSequenceId GUARDED_BY(::cs_main) = SEQ_ID_INIT_FROM_DISK + 1;
    /** Decreasing counter (used by subsequent preciousblock calls). */
    int32_t nBlockReverseSequenceId = -1;
    /** chainwork for the last block that preciousblock has been applied to. */
    arith_uint256 nLastPreciousChainwork = 0;

    // Reset the memory-only sequence counters we use to track block arrival
    // (used by tests to reset state)
    void ResetBlockSequenceCounters() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        nBlockSequenceId = SEQ_ID_INIT_FROM_DISK + 1;
        nBlockReverseSequenceId = -1;
    }


    /** Best header we've seen so far for which the block is not known to be invalid
        (used, among others, for getheaders queries' starting points).
        In case of multiple best headers with the same work, it could point to any
        because CBlockIndexWorkComparator tiebreaker rules are not applied. */
    CBlockIndex* m_best_header GUARDED_BY(::cs_main){nullptr};

    //! The total number of bytes available for us to use across all in-memory
    //! coins caches. This will be split somehow across chainstates.
    size_t m_total_coinstip_cache{0};
    //
    //! The total number of bytes available for us to use across all leveldb
    //! coins databases. This will be split somehow across chainstates.
    size_t m_total_coinsdb_cache{0};

    /// Ensures a genesis block is in the block tree, possibly writing one to disk.
    [[nodiscard]] bool LoadGenesisBlock();

    //! Instantiate a new chainstate.
    //!
    //! @param[in] mempool              The mempool to pass to the chainstate
    //                                  constructor
    Chainstate& InitializeChainstate(CTxMemPool* mempool) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Construct and activate a Chainstate on the basis of UTXO snapshot data.
    //!
    //! Steps:
    //!
    //! - Initialize an unused Chainstate.
    //! - Load its `CoinsViews` contents from `coins_file`.
    //! - Verify that the hash of the resulting coinsdb matches the expected hash
    //!   per assumeutxo chain parameters.
    //! - Wait for our headers chain to include the base block of the snapshot.
    //! - "Fast forward" the tip of the new chainstate to the base of the snapshot.
    //! - Construct the new Chainstate and add it to m_chainstates.
    [[nodiscard]] util::Result<CBlockIndex*> ActivateSnapshot(
        AutoFile& coins_file, const node::SnapshotMetadata& metadata, bool in_memory);

    //! Try to validate an assumeutxo snapshot by using a validated historical
    //! chainstate targeted at the snapshot block. When the target block is
    //! reached, the UTXO hash is computed and saved to
    //! `validated_cs.m_target_utxohash`, and `unvalidated_cs.m_assumeutxo` will
    //! be updated from UNVALIDATED to either VALIDATED or INVALID depending on
    //! whether the hash matches. The INVALID case should not happen in practice
    //! because the software should refuse to load unrecognized snapshots, but
    //! if it does happen, it is a fatal error.
    SnapshotCompletionResult MaybeValidateSnapshot(Chainstate& validated_cs, Chainstate& unvalidated_cs) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Return current chainstate targeting the most-work, network tip.
    Chainstate& CurrentChainstate() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex())
    {
        for (auto& cs : m_chainstates) {
            if (cs && cs->m_assumeutxo != Assumeutxo::INVALID && !cs->TargetBlockHash()) return *cs;
        }
        abort();
    }

    //! Return historical chainstate targeting a specific block, if any.
    Chainstate* HistoricalChainstate() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex())
    {
        for (auto& cs : m_chainstates) {
            if (cs && cs->m_assumeutxo != Assumeutxo::INVALID && cs->TargetBlockHash() && !cs->m_target_utxohash) return cs.get();
        }
        return nullptr;
    }

    //! Return fully validated chainstate that should be used for indexing, to
    //! support indexes that need to index blocks in order and can't start from
    //! the snapshot block.
    Chainstate& ValidatedChainstate() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex())
    {
        for (auto* cs : {&CurrentChainstate(), HistoricalChainstate()}) {
            if (cs && cs->m_assumeutxo == Assumeutxo::VALIDATED) return *cs;
        }
        abort();
    }

    //! Remove a chainstate.
    std::unique_ptr<Chainstate> RemoveChainstate(Chainstate& chainstate) EXCLUSIVE_LOCKS_REQUIRED(GetMutex())
    {
        auto it{std::find_if(m_chainstates.begin(), m_chainstates.end(), [&](auto& cs) { return cs.get() == &chainstate; })};
        if (it != m_chainstates.end()) {
            auto ret{std::move(*it)};
            m_chainstates.erase(it);
            return ret;
        }
        return nullptr;
    }

    //! Alternatives to CurrentChainstate() used by older code to query latest
    //! chainstate information without locking cs_main. Newer code should avoid
    //! querying ChainstateManager and use Chainstate objects directly, or
    //! should use CurrentChainstate() instead.
    //! @{
    Chainstate& ActiveChainstate() const;
    CChain& ActiveChain() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) { return ActiveChainstate().m_chain; }
    int ActiveHeight() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) { return ActiveChain().Height(); }
    CBlockIndex* ActiveTip() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) { return ActiveChain().Tip(); }
    //! @}

    /**
     * Update and possibly latch the IBD status.
     *
     * If block loading has finished and the current chain tip has enough work
     * and is recent, set `m_cached_is_ibd` to false. This function never sets
     * the flag back to true.
     *
     * This should be called after operations that may affect IBD exit
     * conditions (e.g. after updating the active chain tip, or after
     * `ImportBlocks()` finishes).
     */
    void UpdateIBDStatus() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    node::BlockMap& BlockIndex() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return m_blockman.m_block_index;
    }

    /**
     * Track versionbit status
     */
    mutable VersionBitsCache m_versionbitscache;

    /** Check whether we are doing an initial block download (synchronizing from disk or network) */
    bool IsInitialBlockDownload() const noexcept;

    /** The latch as the mode it selects, for an operation to read once where it starts (node/sync_mode.h). */
    node::SyncMode SyncModeNow() const noexcept { return node::SyncModeOf(IsInitialBlockDownload()); }

    /** Guess verification progress (as a fraction between 0.0=genesis and 1.0=current tip).
    * This is also the case in the assumeutxo context, meaning that the progress reported for
    * the snapshot chainstate may suggest that all historical blocks have already been verified
    * even though that may not actually be the case. */
    double GuessVerificationProgress(const CBlockIndex* pindex) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());

    /** Guess background verification progress in case assume-utxo was used (as a fraction between 0.0=genesis and 1.0=snapshot blocks). */
    double GetBackgroundVerificationProgress(const CBlockIndex& pindex) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());

    /**
     * Import blocks from an external file
     *
     * During reindexing, this function is called for each block file (datadir/blocks/blk?????.dat).
     * It reads all blocks contained in the given file and attempts to process them (add them to the
     * block index). The blocks may be out of order within each file and across files. Often this
     * function reads a block but finds that its parent hasn't been read yet, so the block can't be
     * processed yet. The function will add an entry to the blocks_with_unknown_parent map (which is
     * passed as an argument), so that when the block's parent is later read and processed, this
     * function can re-read the child block from disk and process it.
     *
     * Because a block's parent may be in a later file, not just later in the same file, the
     * blocks_with_unknown_parent map must be passed in and out with each call. It's a multimap,
     * rather than just a map, because multiple blocks may have the same parent (when chain splits
     * or stale blocks exist). It maps from parent-hash to child-disk-position.
     *
     * This function can also be used to read blocks from user-specified block files using the
     * -loadblock= option. There's no unknown-parent tracking, so the last two arguments are omitted.
     *
     *
     * @param[in]     file_in                       File containing blocks to read
     * @param[in]     dbp                           (optional) Disk block position (only for reindex)
     * @param[in,out] blocks_with_unknown_parent    (optional) Map of disk positions for blocks with
     *                                              unknown parent, key is parent block hash
     *                                              (only used for reindex)
     * */
    void LoadExternalBlockFile(
        AutoFile& file_in,
        FlatFilePos* dbp = nullptr,
        std::multimap<uint256, FlatFilePos>* blocks_with_unknown_parent = nullptr);

    /**
     * Process an incoming block. Unless `activate_async` is set, this only returns
     * after the best known valid block is made active. Note that it does not,
     * however, guarantee that the specific block passed to it has been checked
     * for validity!
     *
     * If you want to *possibly* get feedback on whether block is valid, you must
     * install a CValidationInterface (see validationinterface.h) - this will have
     * its BlockChecked method called whenever *any* block completes validation.
     *
     * Note that we guarantee that either the proof-of-work is valid on block, or
     * (and possibly also) BlockChecked will have been called.
     *
     * May not be called in a validationinterface callback.
     *
     * @param[in]   block The block we want to process.
     * @param[in]   force_processing Process this block even if unrequested; used for non-network block sources.
     * @param[in]   min_pow_checked  True if proof-of-work anti-DoS checks have
     *                               been done by caller for headers chain
     *                               (note: only affects headers acceptance; if
     *                               block header is already present in block
     *                               index then this parameter has no effect)
     * @param[out]  new_block A boolean which is set to indicate if the block was first received via this call
     * @param[in]   activate_async Return once the block is accepted and stored, and leave making the best
     *                             chain active to the connector thread (SignalConnector()), which reports
     *                             through the validation interface as a synchronous call does.
     * @returns     If the block was processed, independently of block validity
     */
    bool ProcessNewBlock(const std::shared_ptr<const CBlock>& block, bool force_processing, bool min_pow_checked, bool* new_block, bool activate_async = false) LOCKS_EXCLUDED(cs_main) EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex);

    /**
     * The connector thread runs ActivateBestChain() for the chainstates whenever it is signalled, so that the
     * thread that accepted a block need not also connect it (ProcessNewBlock() with `activate_async`).
     * `block`, the block just accepted, goes to the next activation as the block in hand, so that a new
     * block at the tip is connected from memory and not read back. The thread is started on the first signal
     * and stopped with this object. `for_active`: the block is one the active chainstate may connect, not
     * one at or below a snapshot's base (WaitForPendingBlocks()).
     */
    void SignalConnector(std::shared_ptr<const CBlock> block = nullptr, bool for_active = true) EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex);
    //! The ticket of the block handed to the connector last, and whether every block up to a ticket's is Done
    //! (node/handoff_machine.h). A ping is answered once the blocks handed over before it are
    //! (ProcessMessages()), so that a peer never sees a view older than the blocks it has already sent.
    node::HandOffTicket LatestHandOff() const EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex);
    bool HandOffsDone(const node::HandOffTicket& ticket) const EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex);
    /**
     * Wait until the blocks handed to the connector so far that the active chainstate may connect are
     * DoneActive (node/handoff_machine.h), or the connector is stopped (precondition O1). Called by the
     * message handler before it takes cs_main for a peer's transaction and before it judges a compact
     * block's distance from the tip. Not by BroadcastTransaction(): its callers may hold a wallet's lock,
     * which the scheduler needs to drain the validation queue the connector waits for
     * (LimitValidationInterfaceQueue()). Holding g_msgproc_mutex while waiting is safe: only the message
     * handler thread takes it. Not in initial block download: `mode` is the caller's copy, read where its
     * operation started.
     */
    void WaitForPendingBlocks(node::SyncMode mode) EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex) LOCKS_EXCLUDED(::cs_main);
    /**
     * Whether the historical chainstate's activation is to give way: the node is out of initial block
     * download (`mode`, the copy of the caller's step) and a block the active chainstate may connect was
     * handed to the connector and is not taken yet, which the message handler waits for
     * (WaitForPendingBlocks()). In initial block download nobody waits, and giving way to every block
     * would starve the historical chainstate. Once true it stays true until the connector takes.
     */
    bool YieldHistoricalActivation(node::SyncMode mode) const EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex);
    //! Whether a block handed to the connector is not Done yet (Queued, Taken or DoneActive). Blocks accepted
    //! but not yet connected move the active tip once it runs; callers that measure from the tip may wait for
    //! that.
    bool AnyHandOffPending() const EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex);
    //! Stop the connector thread and wait for it: its current pass ends at the next publication once the
    //! node is interrupted, the window drained. Called at shutdown before the chainstate is flushed and
    //! its views are reset, and again, idempotently, by the destructor. The connector does not start
    //! again: a block handed over from here on is stored and not activated (SignalConnector()).
    void StopConnector() EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex);

    /**
     * Process incoming block headers.
     *
     * May not be called in a
     * validationinterface callback.
     *
     * @param[in]  headers The block headers themselves
     * @param[in]  min_pow_checked  True if proof-of-work anti-DoS checks have been done by caller for headers chain
     * @param[out] state This may be set to an Error state if any error occurred processing them
     * @param[out] ppindex If set, the pointer will be set to point to the last new block index object for the given headers
     * @returns false if AcceptBlockHeader fails on any of the headers, true otherwise (including if headers were already known)
     */
    bool ProcessNewBlockHeaders(std::span<const CBlockHeader> headers, bool min_pow_checked, BlockValidationState& state, const CBlockIndex** ppindex = nullptr) LOCKS_EXCLUDED(cs_main);

    /**
     * Sufficiently validate a block for disk storage (and store on disk).
     *
     * @param[in]   pblock          The block we want to process.
     * @param[in]   fRequested      Whether we requested this block from a
     *                              peer.
     * @param[in]   dbp             The location on disk, if we are importing
     *                              this block from prior storage.
     * @param[in]   min_pow_checked True if proof-of-work anti-DoS checks have
     *                              been done by caller for headers chain
     *
     * @param[out]  state       The state of the block validation.
     * @param[out]  ppindex     Optional return parameter to get the
     *                          CBlockIndex pointer for this block.
     * @param[out]  fNewBlock   Optional return parameter to indicate if the
     *                          block is new to our storage.
     *
     * @returns   False if the block or header is invalid, or if saving to disk fails (likely a fatal error); true otherwise.
     */
    bool AcceptBlock(const std::shared_ptr<const CBlock>& pblock, BlockValidationState& state, CBlockIndex** ppindex, bool fRequested, const FlatFilePos* dbp, bool* fNewBlock, bool min_pow_checked) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * AcceptBlock() split around the file write, so that ProcessNewBlock()
     * can write the block with cs_main released. The first part does
     * everything up to and including claiming the block's position on disk;
     * if it returns true with prepared.store set, the caller writes the
     * block at prepared.pos (BlockManager::WriteBlockAt, no lock needed)
     * and then calls FinishAcceptBlock(), which updates the block index and
     * runs the post-acceptance bookkeeping. With a dbp the block is already
     * on disk and prepared.write is false. The parameters are those of
     * AcceptBlock().
     */
    struct PreparedBlock {
        CBlockIndex* pindex{nullptr};
        FlatFilePos pos;
        //! Storage is pending: FinishAcceptBlock() must be called.
        bool store{false};
        //! The block must be written at pos before FinishAcceptBlock().
        bool write{false};
        //! The block's checks are pending: CheckAcceptBlock() then PrepareAcceptBlockStorage() must be called.
        bool check{false};
        //! Set by CheckAcceptBlock(): CheckBlock() itself failed (as opposed to the contextual checks).
        bool check_block_failed{false};
        //! For the checks, built under the lock.
        std::optional<BlockValidationContext> ctx;
    };
    /**
     * The first part in three, so that the block's own checks run with no
     * lock held: PrepareAcceptBlockHeader() accepts the header and decides
     * whether the block is wanted (prepared.check); CheckAcceptBlock() runs
     * CheckBlock() and ContextualCheckBlock(); PrepareAcceptBlockStorage()
     * records an invalid block, relays a valid one and claims its place on
     * disk. With invalidate_on_check_block_failure false, a block that fails
     * CheckBlock() is rejected without being marked invalid.
     */
    bool PrepareAcceptBlockHeader(const CBlock& block, BlockValidationState& state, CBlockIndex** ppindex, bool fRequested, bool* fNewBlock, bool min_pow_checked, PreparedBlock& prepared) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool CheckAcceptBlock(const CBlock& block, BlockValidationState& state, PreparedBlock& prepared) const;
    bool PrepareAcceptBlockStorage(const std::shared_ptr<const CBlock>& pblock, BlockValidationState& state, bool checks_ok, bool invalidate_on_check_block_failure, const FlatFilePos* dbp, bool* fNewBlock, PreparedBlock& prepared) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool FinishAcceptBlock(const CBlock& block, BlockValidationState& state, PreparedBlock& prepared, bool written) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Block-index transition: the block's scripts were verified (or accepted
    //! under assumevalid), so it is BLOCK_VALID_SCRIPTS from now on.
    void MarkBlockScriptsValid(CBlockIndex& block) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    void ReceivedBlockTransactions(const CBlock& block, CBlockIndex* pindexNew, const FlatFilePos& pos) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Try to add a transaction to the memory pool.
     *
     * @param[in]  tx              The transaction to submit for mempool acceptance.
     * @param[in]  test_accept     When true, run validation checks but don't submit to mempool.
     */
    [[nodiscard]] MempoolAcceptResult ProcessTransaction(const CTransactionRef& tx, bool test_accept=false)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Load the block tree and coins database from disk, initializing state if we're running with -reindex
    bool LoadBlockIndex() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Check to see if caches are out of balance and if so, call
    //! ResizeCoinsCaches() as needed.
    void MaybeRebalanceCaches() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Update uncommitted block structures (currently: only the witness reserved
     * value). This is safe for submitted blocks as long as they honor
     * default_witness_commitment from the template.
     */
    void UpdateUncommittedBlockStructures(CBlock& block, const CBlockIndex* pindexPrev) const;

    /** Produce the necessary coinbase commitment for a block (modifies the hash, don't call for mined blocks). */
    void GenerateCoinbaseCommitment(CBlock& block, const CBlockIndex* pindexPrev) const;

    /** This is used by net_processing to report pre-synchronization progress of headers, as
     *  headers are not yet fed to validation during that time, but validation is (for now)
     *  responsible for logging and signalling through NotifyHeaderTip, so it needs this
     *  information. */
    void ReportHeadersPresync(int64_t height, int64_t timestamp);

    //! When starting up, search the datadir for a chainstate based on a UTXO
    //! snapshot that is in the process of being validated and load it if found.
    //! Return pointer to the Chainstate if it is loaded.
    Chainstate* LoadAssumeutxoChainstate() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Add new chainstate.
    Chainstate& AddChainstate(std::unique_ptr<Chainstate> chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    void ResetChainstates() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Remove the chainstate and all on-disk artifacts.
    //! Used when reindex{-chainstate} is called during snapshot use.
    [[nodiscard]] bool DeleteChainstate(Chainstate& chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! If we have validated a snapshot chain during this runtime, copy its
    //! chainstate directory over to the main `chainstate` location, completing
    //! validation of the snapshot.
    //!
    //! If the cleanup succeeds, the caller will need to ensure chainstates are
    //! reinitialized, since ResetChainstates() will be called before leveldb
    //! directories are moved or deleted.
    //!
    //! @sa node/chainstate:LoadChainstate()
    bool ValidatedSnapshotCleanup(Chainstate& validated_cs, Chainstate& unvalidated_cs) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Get range of historical blocks to download.
    std::optional<std::pair<const CBlockIndex*, const CBlockIndex*>> GetHistoricalBlockRange() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Call ActivateBestChain() on every chainstate.
    util::Result<void> ActivateBestChains() LOCKS_EXCLUDED(::cs_main);

    //! If, due to invalidation / reconsideration of blocks, the previous
    //! best header is no longer valid / guaranteed to be the most-work
    //! header in our block-index not known to be invalid, recalculate it.
    void RecalculateBestHeader() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Returns how many blocks the best header is ahead of the current tip,
    //! or nullopt if the best header does not extend the tip.
    std::optional<int> BlocksAheadOfTip() const LOCKS_EXCLUDED(::cs_main);

    CCheckQueue<CScriptCheck>& GetCheckQueue() { return m_script_check_queue; }
    //! The connect pool: block appliers of the window and the blocks received from peers during initial
    //! block download, input prefetchers at the tip (sized by -prevoutfetchthreads). Shared by every
    //! chainstate's views.
    const util::NotNullSharedPtr<ThreadPool> m_connect_pool;
    /**
     * The free worker. The connector waits for the connect pool's workers with cs_main held
     * (Chainstate::DrainWindow()). The work it waits for, a block's validation, takes no cs_main, but it
     * needs a worker, and the pool also runs tasks that wait for cs_main: the receive tasks of net
     * processing (PeerManagerImpl::HandOffBlock()). Whoever gives the pool such a task keeps to
     * LockTaskLimit(), one fewer than the workers, so at least one worker is in no such task and the waits
     * end.
     */
    int LockTaskLimit() const;
    //! The blocks each thread built in a task on the connect pool, back from their last holder (NewBlock()).
    const std::unique_ptr<BlockBins> m_block_bins;
    /**
     * A block to be read or received, whose destruction runs on the connect pool instead of the thread that
     * drops its last reference (for a connected block the scheduler, in its BlockConnected callback). The
     * deleter deletes in place when the pool has no workers.
     *
     * A block built by a task on the pool (`on_pool`) goes back to the thread that built it, which frees
     * it before it builds its next one. What is left with a thread that builds no more is freed by
     * ActivateBestChain() outside initial block download.
     */
    std::shared_ptr<CBlock> NewBlock(bool on_pool = false) const;
    //! How many blocks the window validates at once: -connectwindow, or twice the connect pool's worker count.
    int ConnectWindow() const;

    ~ChainstateManager() EXCLUSIVE_LOCKS_REQUIRED(!m_connector_mutex);

    //! List of chainstates. Note: in general, it is not safe to delete
    //! Chainstate objects once they are added to this list because there is no
    //! mutex that can be locked to prevent Chainstate pointers from being used
    //! while they are deleted. (cs_main doesn't work because it is too narrow
    //! and is released in the middle of Chainstate::ActivateBestChain to let
    //! notifications be processed. m_chainstate_mutex doesn't work because it
    //! is not locked at other times when the chainstate is in use.)
    std::vector<std::unique_ptr<Chainstate>> m_chainstates GUARDED_BY(::cs_main);
};

/** Deployment* info via ChainstateManager */
template<typename DEP>
bool DeploymentActiveAfter(const CBlockIndex* pindexPrev, const ChainstateManager& chainman, DEP dep)
{
    return DeploymentActiveAfter(pindexPrev, chainman.GetConsensus(), dep, chainman.m_versionbitscache);
}

template<typename DEP>
bool DeploymentActiveAt(const CBlockIndex& index, const ChainstateManager& chainman, DEP dep)
{
    return DeploymentActiveAt(index, chainman.GetConsensus(), dep, chainman.m_versionbitscache);
}

template<typename DEP>
bool DeploymentEnabled(const ChainstateManager& chainman, DEP dep)
{
    return DeploymentEnabled(chainman.GetConsensus(), dep);
}

/** Identifies blocks that overwrote an existing coinbase output in the UTXO set (see BIP30) */
bool IsBIP30Repeat(const CBlockIndex& block_index);
bool IsBIP30Repeat(const uint256& block_hash, int block_height);

/** Identifies blocks which coinbase output was subsequently overwritten in the UTXO set (see BIP30) */
bool IsBIP30Unspendable(const uint256& block_hash, int block_height);

/** Set of script verify flags that may be enforced at the consensus level
 *  (for some combination of chain params and chain state)
 */
inline constexpr script_verify_flags CONSENSUS_SCRIPT_VERIFY_FLAGS{
    SCRIPT_VERIFY_NONE
    | SCRIPT_VERIFY_P2SH
    | SCRIPT_VERIFY_WITNESS
    | SCRIPT_VERIFY_TAPROOT
    | SCRIPT_VERIFY_DERSIG
    | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY
    | SCRIPT_VERIFY_CHECKSEQUENCEVERIFY
    | SCRIPT_VERIFY_NULLDUMMY
};

/** Evaluate which deployments are active for the block following `prev` (nullptr for genesis). */
DeploymentFlags EvaluateDeploymentFlags(const CBlockIndex* prev, const ChainstateManager& chainman);

/**
 * Undo the effects of `block` on `view`, using its undo data. Pure apart from
 * the view it mutates: no disk access, no node state. The undo data is
 * consumed. On DISCONNECT_UNCLEAN the view has been rolled back but was
 * inconsistent with the block; on DISCONNECT_FAILED the undo data did not
 * match the block or the view, and the view may be partially modified.
 * `block_hash` and `height` are the block's own.
 */
DisconnectResult DisconnectBlock(const CBlock& block, CBlockUndo&& block_undo, const uint256& block_hash, int height, CCoinsViewCache& view);

/**
 * Returns the script flags which should be checked for a given block
 * (a subset of CONSENSUS_SCRIPT_VERIFY_FLAGS). Pure function of the block
 * hash (for the historical exceptions), the active deployments and the
 * consensus parameters.
 */
script_verify_flags GetBlockScriptFlags(const uint256& block_hash, const DeploymentFlags& deployments, const Consensus::Params& params);

// Convenience wrapper evaluating the deployments for `block_index` itself.
script_verify_flags GetBlockScriptFlags(const CBlockIndex& block_index, const ChainstateManager& chainman);

/** The context CheckInputScripts is called in, which decides how a
 *  script failure is reported: as a consensus failure for blocks,
 *  or as a standardness failure for the mempool. The flags alone can't
 *  determine this, as the mempool may check with consensus flags only
 *  (-acceptnonstdtxn).
 */
enum class CheckInputScriptsFor {
    Block,
    Mempool,
};

bool CheckInputScripts(const CTransaction& tx, TxValidationState& state,
                       const CCoinsViewCache& inputs,
                       script_verify_flags flags,
                       CheckInputScriptsFor check_for,
                       bool cacheSigStore,
                       bool cacheFullScriptStore, PrecomputedTransactionData& txdata,
                       ValidationCache& validation_cache,
                       std::vector<CScriptCheck>* pvChecks = nullptr);

#endif // BITCOIN_VALIDATION_H
