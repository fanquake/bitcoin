// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/messages.h>

#include <common/types.h>
#include <node/types.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/fees.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using node::TransactionError;
using util::Join;

namespace common {
std::string StringForFeeReason(FeeReason reason)
{
    static const std::map<FeeReason, std::string> fee_reason_strings = {
        {FeeReason::FEE_RATE_ESTIMATOR, "Fee Rate Estimator"},
        {FeeReason::MEMPOOL_MIN, "Mempool Min Fee"},
        {FeeReason::USER_SPECIFIED, "User Specified Fee"},
        {FeeReason::FALLBACK, "Fallback fee"},
        {FeeReason::REQUIRED, "Minimum Required Fee"},
    };
    auto reason_string = fee_reason_strings.find(reason);

    if (reason_string == fee_reason_strings.end()) return "Unknown";

    return reason_string->second;
}

const std::vector<std::pair<std::string, FeeEstimateMode>>& FeeModeMap()
{
    static const std::vector<std::pair<std::string, FeeEstimateMode>> FEE_MODES = {
        {"unset", FeeEstimateMode::UNSET},
        {"economical", FeeEstimateMode::ECONOMICAL},
        {"conservative", FeeEstimateMode::CONSERVATIVE},
    };
    return FEE_MODES;
}

std::string FeeModeInfo(const std::pair<std::string, FeeEstimateMode>& mode, std::string& default_info)
{
    switch (mode.second) {
        case FeeEstimateMode::UNSET:
            return strprintf("%s means no mode set (%s). \n", mode.first, default_info);
        case FeeEstimateMode::ECONOMICAL:
            return strprintf("%s mode potentially returns a lower fee rate estimate.\n", mode.first);
        case FeeEstimateMode::CONSERVATIVE:
            return strprintf("%s potentially returns a higher fee rate estimate.\n", mode.first);
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

std::string FeeModesDetail(std::string default_info)
{
    std::string info;
    for (const auto& fee_mode : FeeModeMap()) {
        info += FeeModeInfo(fee_mode, default_info);
    }
    return strprintf("%s \n%s", FeeModes(", "), info);
}

std::string FeeModes(const std::string& delimiter)
{
    return Join(FeeModeMap(), delimiter, [&](const std::pair<std::string, FeeEstimateMode>& i) { return i.first; });
}

std::string InvalidEstimateModeErrorMessage()
{
    return "Invalid estimate_mode parameter, must be one of: \"" + FeeModes("\", \"") + "\"";
}

bool FeeModeFromString(std::string_view mode_string, FeeEstimateMode& fee_estimate_mode)
{
    auto searchkey = ToUpper(mode_string);
    for (const auto& pair : FeeModeMap()) {
        if (ToUpper(pair.first) == searchkey) {
            fee_estimate_mode = pair.second;
            return true;
        }
    }
    return false;
}

std::string PSBTErrorString(PSBTError err)
{
    switch (err) {
        case PSBTError::MISSING_INPUTS:
            return "Inputs missing or spent";
        case PSBTError::SIGHASH_MISMATCH:
            return "Specified sighash value does not match value stored in PSBT";
        case PSBTError::EXTERNAL_SIGNER_NOT_FOUND:
            return "External signer not found";
        case PSBTError::EXTERNAL_SIGNER_FAILED:
            return "External signer failed to sign";
        case PSBTError::UNSUPPORTED:
            return "Signer does not support PSBT";
        case PSBTError::INCOMPLETE:
            return "Input needs additional signatures or other data";
        case PSBTError::INVALID_TX:
            return "The transaction cannot be valid";
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

std::string TransactionErrorString(const TransactionError err)
{
    switch (err) {
        case TransactionError::OK:
            return "No error";
        case TransactionError::MISSING_INPUTS:
            return "Inputs missing or spent";
        case TransactionError::ALREADY_IN_UTXO_SET:
            return "Transaction outputs already in utxo set";
        case TransactionError::MEMPOOL_REJECTED:
            return "Transaction rejected by mempool";
        case TransactionError::MEMPOOL_ERROR:
            return "Mempool internal error";
        case TransactionError::MAX_FEE_EXCEEDED:
            return "Fee exceeds maximum configured by user (e.g. -maxtxfee, maxfeerate)";
        case TransactionError::MAX_BURN_EXCEEDED:
            return "Unspendable output exceeds maximum configured by user (maxburnamount)";
        case TransactionError::INVALID_PACKAGE:
            return "Transaction rejected due to invalid package";
        case TransactionError::PRIVATE_BROADCAST_FULL:
            return "Private broadcast queue is full";
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

std::string ResolveErrMsg(const std::string& optname, const std::string& strBind)
{
    return strprintf("Cannot resolve -%s address: '%s'", optname, strBind);
}

std::string InvalidPortErrMsg(const std::string& optname, const std::string& invalid_value)
{
    return strprintf("Invalid port specified in %s: '%s'", optname, invalid_value);
}

std::string AmountHighWarn(const std::string& optname)
{
    return strprintf("%s is set very high!", optname);
}

std::string AmountErrMsg(const std::string& optname, const std::string& strValue)
{
    return strprintf("Invalid amount for -%s=<amount>: '%s'", optname, strValue);
}
} // namespace common
