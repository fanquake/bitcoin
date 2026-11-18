// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SCRIPT_SIGCACHE_H
#define BITCOIN_SCRIPT_SIGCACHE_H

#include <consensus/amount.h>
#include <crypto/sha256.h>
#include <pubkey.h>
#include <cuckoocache.h>
#include <script/interpreter.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/hasher.h>

#include <cstddef>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

class CPubKey;
class CTransaction;
class XOnlyPubKey;

// DoS prevention: limit cache size to 32MiB (over 1000000 entries on 64-bit
// systems). Due to how we count cache size, actual memory usage is slightly
// more (~32.25 MiB)
inline constexpr size_t DEFAULT_VALIDATION_CACHE_BYTES{32_MiB};
inline constexpr size_t DEFAULT_SIGNATURE_CACHE_BYTES{DEFAULT_VALIDATION_CACHE_BYTES / 2};
inline constexpr size_t DEFAULT_SCRIPT_EXECUTION_CACHE_BYTES{DEFAULT_VALIDATION_CACHE_BYTES / 2};
static_assert(DEFAULT_VALIDATION_CACHE_BYTES == DEFAULT_SIGNATURE_CACHE_BYTES + DEFAULT_SCRIPT_EXECUTION_CACHE_BYTES);

/**
 * Valid signature cache, to avoid doing expensive ECDSA signature checking
 * twice for every transaction (once when accepted into memory pool, and
 * again when accepted into the block chain)
 */
class SignatureCache
{
private:
    //! Entries are SHA256(nonce || 'E' or 'S' || 31 zero bytes || signature hash || public key || signature):
    CSHA256 m_salted_hasher_ecdsa;
    CSHA256 m_salted_hasher_schnorr;
    typedef CuckooCache::cache<uint256, SignatureCacheHasher> map_type;
    map_type setValid;
    std::shared_mutex cs_sigcache;

public:
    SignatureCache(size_t max_size_bytes);

    SignatureCache(const SignatureCache&) = delete;
    SignatureCache& operator=(const SignatureCache&) = delete;

    void ComputeEntryECDSA(uint256& entry, const uint256 &hash, const std::vector<unsigned char>& vchSig, const CPubKey& pubkey) const;

    void ComputeEntrySchnorr(uint256& entry, const uint256 &hash, std::span<const unsigned char> sig, const XOnlyPubKey& pubkey) const;

    bool Get(const uint256& entry, bool erase);

    void Set(const uint256& entry);
};

/** Signature and Taproot commitment checks deferred by CachingTransactionSignatureChecker. */
struct DeferredSignatureChecks {
    enum class Kind : uint8_t { ECDSA, SCHNORR, TAP_TWEAK };
    struct Check {
        //! Identifies the check: its signature cache entry, or for a Taproot commitment a hash of its inputs.
        uint256 id;
        Kind kind;
        //! Position in the batch of its kind.
        uint32_t index;
    };

    ECDSABatch ecdsa;
    SchnorrBatch schnorr;
    TapTweakBatch tap_tweaks;
    //! Every deferred check, in the order they were added.
    std::vector<Check> checks;
    /** Outcomes of checks verified earlier, by id, reported instead of being deferred again. */
    std::unordered_map<uint256, bool, SaltedUint256Hasher> verified;

    void clear();
    /** Verify every deferred check. Element i of results is whether checks[i] succeeded. */
    void Verify(std::vector<unsigned char>& results);
};

class CachingTransactionSignatureChecker : public TransactionSignatureChecker
{
private:
    bool store;
    SignatureCache& m_signature_cache;
    /** If set, signature checks that miss the signature cache, and Taproot commitment checks, are looked up in its
     *  verified outcomes, or else added to it instead of being verified and reported as successful. A script's
     *  outcome then only holds if every check it deferred turns out to be valid. */
    DeferredSignatureChecks* m_deferred;

public:
    CachingTransactionSignatureChecker(const CTransaction* txToIn, unsigned int nInIn, const CAmount& amountIn, bool storeIn, SignatureCache& signature_cache, PrecomputedTransactionData& txdataIn, DeferredSignatureChecks* deferred = nullptr) : TransactionSignatureChecker(txToIn, nInIn, amountIn, txdataIn, MissingDataBehavior::ASSERT_FAIL), store(storeIn), m_signature_cache(signature_cache), m_deferred(deferred) {}

    bool VerifyECDSASignature(const std::vector<unsigned char>& vchSig, const CPubKey& vchPubKey, const uint256& sighash) const override;
    bool VerifySchnorrSignature(std::span<const unsigned char> sig, const XOnlyPubKey& pubkey, const uint256& sighash) const override;
    bool CheckTapTweak(const XOnlyPubKey& output, const XOnlyPubKey& internal, const uint256& merkle_root, bool parity) const override;
};

#endif // BITCOIN_SCRIPT_SIGCACHE_H
