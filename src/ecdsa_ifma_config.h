/***********************************************************************
 * AVX-512 IFMA multi-buffer ECDSA verification configuration.         *
 * Distributed under the MIT software license, see the accompanying    *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.*
 ***********************************************************************/

#ifndef SECP256K1_ECDSA_IFMA_CONFIG_H
#define SECP256K1_ECDSA_IFMA_CONFIG_H

#define SECP256K1_ECDSA_IFMA_LANES 8
#define SECP256K1_ECDSA_IFMA_WINDOWS 33
#define SECP256K1_ECDSA_IFMA_Q_TABLE 8
#define SECP256K1_ECDSA_IFMA_JOINT_TABLE 256
#define SECP256K1_ECDSA_IFMA_TILE 128

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__) \
    && defined(SECP256K1_WIDEMUL_INT128) \
    && !defined(EXHAUSTIVE_TEST_ORDER)
#define SECP256K1_ECDSA_VERIFY_MANY_IFMA 1
#define SECP256K1_ECDSA_IFMA_TARGET \
    __attribute__((target("avx512f,avx512dq,avx512ifma")))
#endif

#endif /* SECP256K1_ECDSA_IFMA_CONFIG_H */
