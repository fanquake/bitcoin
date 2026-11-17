/***********************************************************************
 * Copyright (c) 2026 The secp256k1 developers                         *
 * Distributed under the MIT software license, see the accompanying    *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.*
 ***********************************************************************/

#ifndef SECP256K1_PRECOMPUTED_ECDSA_IFMA_H
#define SECP256K1_PRECOMPUTED_ECDSA_IFMA_H

#include <stdint.h>

#include "ecdsa_ifma_config.h"
#include "util_local_visibility.h"

#ifdef SECP256K1_ECDSA_VERIFY_MANY_IFMA
SECP256K1_LOCAL_VAR const uint64_t secp256k1_ecdsa_ifma_gx[5];
SECP256K1_LOCAL_VAR const uint64_t secp256k1_ecdsa_ifma_gy[5];
SECP256K1_LOCAL_VAR const uint64_t secp256k1_ecdsa_ifma_gphix[5];
SECP256K1_LOCAL_VAR const uint64_t secp256k1_ecdsa_ifma_gphiy[5];
SECP256K1_LOCAL_VAR const uint64_t secp256k1_ecdsa_ifma_joint_x[5][SECP256K1_ECDSA_IFMA_JOINT_TABLE];
SECP256K1_LOCAL_VAR const uint64_t secp256k1_ecdsa_ifma_joint_y[5][SECP256K1_ECDSA_IFMA_JOINT_TABLE];
#endif

#endif /* SECP256K1_PRECOMPUTED_ECDSA_IFMA_H */
