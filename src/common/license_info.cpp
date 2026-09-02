// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <common/license_info.h>

#include <tinyformat.h>

#include <string>

std::string CopyrightHolders(const std::string& strPrefix)
{
    const auto copyright_devs = strprintf(COPYRIGHT_HOLDERS, COPYRIGHT_HOLDERS_SUBSTITUTION);
    std::string strCopyrightHolders = strPrefix + copyright_devs;

    // Make sure Bitcoin Core copyright is not removed by accident
    if (copyright_devs.find("Bitcoin Core") == std::string::npos) {
        strCopyrightHolders += "\n" + strPrefix + "The Bitcoin Core developers";
    }
    return strCopyrightHolders;
}

std::string LicenseInfo()
{
    const std::string URL_SOURCE_CODE = "<https://github.com/bitcoin/bitcoin>";

    return CopyrightHolders(strprintf("Copyright (C) %i-%i", 2009, COPYRIGHT_YEAR) + " ") + "\n" +
           "\n" +
           strprintf("Please contribute if you find %s useful. "
                       "Visit %s for further information about the software.",
                     CLIENT_NAME, "<" CLIENT_URL ">") +
           "\n" +
           strprintf("The source code is available from %s.", URL_SOURCE_CODE) +
           "\n" +
           "\n" +
           "This is experimental software." + "\n" +
           strprintf("Distributed under the MIT software license, see the accompanying file %s or %s", "COPYING", "<https://opensource.org/license/MIT>") +
           "\n";
}
