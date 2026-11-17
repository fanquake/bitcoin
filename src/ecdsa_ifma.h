/***********************************************************************
 * AVX-512 IFMA multi-buffer ECDSA verification.                       *
 * Distributed under the MIT software license, see the accompanying    *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.*
 ***********************************************************************/

#ifndef SECP256K1_ECDSA_IFMA_H
#define SECP256K1_ECDSA_IFMA_H

#include <immintrin.h>
#include <stdint.h>

#include "ecdsa_ifma_config.h"

typedef struct {
    __m512i n[5];
} secp256k1_ecdsa_ifma_fe8;

typedef struct {
    secp256k1_ecdsa_ifma_fe8 x;
    secp256k1_ecdsa_ifma_fe8 y;
} secp256k1_ecdsa_ifma_aff8;

typedef struct {
    secp256k1_ecdsa_ifma_fe8 x;
    secp256k1_ecdsa_ifma_fe8 y;
    secp256k1_ecdsa_ifma_fe8 z;
} secp256k1_ecdsa_ifma_jac8;

typedef struct {
    uint8_t dmag[4][SECP256K1_ECDSA_IFMA_WINDOWS][SECP256K1_ECDSA_IFMA_LANES];
    uint8_t dneg[4][SECP256K1_ECDSA_IFMA_WINDOWS][SECP256K1_ECDSA_IFMA_LANES];
    uint8_t even[4][SECP256K1_ECDSA_IFMA_LANES];
    uint8_t correction_neg[4][SECP256K1_ECDSA_IFMA_LANES];
    uint8_t eligible[SECP256K1_ECDSA_IFMA_LANES];
    uint8_t r_plus_n_ok[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_ecdsa_ifma_aff8 qtab[SECP256K1_ECDSA_IFMA_Q_TABLE];
    secp256k1_ecdsa_ifma_aff8 qphi[SECP256K1_ECDSA_IFMA_Q_TABLE];
    secp256k1_ecdsa_ifma_fe8 global_z;
    secp256k1_ecdsa_ifma_fe8 iso_z2;
    secp256k1_ecdsa_ifma_fe8 iso_z3;
    secp256k1_ecdsa_ifma_fe8 r;
    secp256k1_ecdsa_ifma_fe8 r_plus_n;
} secp256k1_ecdsa_ifma_prepared8;

#endif /* SECP256K1_ECDSA_IFMA_H */
