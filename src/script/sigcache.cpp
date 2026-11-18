// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/sigcache.h>

#include <crypto/sha256.h>
#include <hash.h>
#include <pubkey.h>
#include <random.h>
#include <script/interpreter.h>
#include <uint256.h>
#include <util/log.h>

#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

SignatureCache::SignatureCache(const size_t max_size_bytes)
{
    uint256 nonce = GetRandHash();
    // We want the nonce to be 64 bytes long to force the hasher to process
    // this chunk, which makes later hash computations more efficient. We
    // just write our 32-byte entropy, and then pad with 'E' for ECDSA and
    // 'S' for Schnorr (followed by 0 bytes).
    static constexpr unsigned char PADDING_ECDSA[32] = {'E'};
    static constexpr unsigned char PADDING_SCHNORR[32] = {'S'};
    m_salted_hasher_ecdsa.Write(nonce.begin(), 32);
    m_salted_hasher_ecdsa.Write(PADDING_ECDSA, 32);
    m_salted_hasher_schnorr.Write(nonce.begin(), 32);
    m_salted_hasher_schnorr.Write(PADDING_SCHNORR, 32);

    const auto [num_elems, approx_size_bytes] = setValid.setup_bytes(max_size_bytes);
    LogInfo("Using %zu MiB out of %zu MiB requested for signature cache, able to store %zu elements",
              approx_size_bytes >> 20, max_size_bytes >> 20, num_elems);
}

void SignatureCache::ComputeEntryECDSA(uint256& entry, const uint256& hash, const std::vector<unsigned char>& vchSig, const CPubKey& pubkey) const
{
    CSHA256 hasher = m_salted_hasher_ecdsa;
    hasher.Write(hash.begin(), 32).Write(pubkey.data(), pubkey.size()).Write(vchSig.data(), vchSig.size()).Finalize(entry.begin());
}

void SignatureCache::ComputeEntrySchnorr(uint256& entry, const uint256& hash, std::span<const unsigned char> sig, const XOnlyPubKey& pubkey) const
{
    CSHA256 hasher = m_salted_hasher_schnorr;
    hasher.Write(hash.begin(), 32).Write(pubkey.data(), pubkey.size()).Write(sig.data(), sig.size()).Finalize(entry.begin());
}

bool SignatureCache::Get(const uint256& entry, const bool erase)
{
    std::shared_lock<std::shared_mutex> lock(cs_sigcache);
    return setValid.contains(entry, erase);
}

void SignatureCache::Set(const uint256& entry)
{
    std::unique_lock<std::shared_mutex> lock(cs_sigcache);
    setValid.insert(entry);
}

bool CachingTransactionSignatureChecker::VerifyECDSASignature(const std::vector<unsigned char>& vchSig, const CPubKey& pubkey, const uint256& sighash) const
{
    uint256 entry;
    m_signature_cache.ComputeEntryECDSA(entry, sighash, vchSig, pubkey);
    if (m_signature_cache.Get(entry, !store))
        return true;
    if (m_deferred) {
        if (const auto it{m_deferred->verified.find(entry)}; it != m_deferred->verified.end()) return it->second;
        if (!m_deferred->ecdsa.Add(pubkey, sighash, vchSig)) return false;
        m_deferred->checks.push_back({entry, DeferredSignatureChecks::Kind::ECDSA, uint32_t(m_deferred->ecdsa.size() - 1)});
        return true;
    }
    if (!TransactionSignatureChecker::VerifyECDSASignature(vchSig, pubkey, sighash))
        return false;
    if (store)
        m_signature_cache.Set(entry);
    return true;
}

bool CachingTransactionSignatureChecker::VerifySchnorrSignature(std::span<const unsigned char> sig, const XOnlyPubKey& pubkey, const uint256& sighash) const
{
    uint256 entry;
    m_signature_cache.ComputeEntrySchnorr(entry, sighash, sig, pubkey);
    if (m_signature_cache.Get(entry, !store)) return true;
    if (m_deferred) {
        if (const auto it{m_deferred->verified.find(entry)}; it != m_deferred->verified.end()) return it->second;
        if (!m_deferred->schnorr.Add(pubkey, sighash, sig)) return false;
        m_deferred->checks.push_back({entry, DeferredSignatureChecks::Kind::SCHNORR, uint32_t(m_deferred->schnorr.size() - 1)});
        return true;
    }
    if (!TransactionSignatureChecker::VerifySchnorrSignature(sig, pubkey, sighash)) return false;
    if (store) m_signature_cache.Set(entry);
    return true;
}

bool CachingTransactionSignatureChecker::CheckTapTweak(const XOnlyPubKey& output, const XOnlyPubKey& internal, const uint256& merkle_root, bool parity) const
{
    if (!m_deferred) return TransactionSignatureChecker::CheckTapTweak(output, internal, merkle_root, parity);
    static const HashWriter HASHER_DEFERRED_TAP_TWEAK{TaggedHash("Bitcoin Core deferred TapTweak check")};
    const uint256 id{(HashWriter{HASHER_DEFERRED_TAP_TWEAK} << output << internal << merkle_root << parity).GetSHA256()};
    if (const auto it{m_deferred->verified.find(id)}; it != m_deferred->verified.end()) return it->second;
    if (!m_deferred->tap_tweaks.Add(output, internal, merkle_root, parity)) return false;
    m_deferred->checks.push_back({id, DeferredSignatureChecks::Kind::TAP_TWEAK, uint32_t(m_deferred->tap_tweaks.size() - 1)});
    return true;
}

void DeferredSignatureChecks::clear()
{
    ecdsa.clear();
    schnorr.clear();
    tap_tweaks.clear();
    checks.clear();
}

void DeferredSignatureChecks::Verify(std::vector<unsigned char>& results)
{
    thread_local std::vector<unsigned char> ecdsa_results, schnorr_results, tap_tweak_results;
    ecdsa.Verify(ecdsa_results);
    schnorr.Verify(schnorr_results);
    tap_tweaks.Verify(tap_tweak_results);
    results.resize(checks.size());
    for (size_t i{0}; i < checks.size(); ++i) {
        switch (checks[i].kind) {
        case Kind::ECDSA: results[i] = ecdsa_results[checks[i].index]; break;
        case Kind::SCHNORR: results[i] = schnorr_results[checks[i].index]; break;
        case Kind::TAP_TWEAK: results[i] = tap_tweak_results[checks[i].index]; break;
        }
    }
}
