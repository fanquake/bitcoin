/***********************************************************************
 * Copyright (c) 2020 Jonas Nick                                       *
 * Distributed under the MIT software license, see the accompanying    *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.*
 ***********************************************************************/

#ifndef SECP256K1_MODULE_EXTRAKEYS_MAIN_H
#define SECP256K1_MODULE_EXTRAKEYS_MAIN_H

#include "../../../include/secp256k1.h"
#include "../../../include/secp256k1_extrakeys.h"
#include "../../util.h"

static SECP256K1_INLINE int secp256k1_xonly_pubkey_load(const secp256k1_context* ctx, secp256k1_ge *ge, const secp256k1_xonly_pubkey *pubkey) {
    return secp256k1_pubkey_load(ctx, ge, (const secp256k1_pubkey *) pubkey);
}

static SECP256K1_INLINE void secp256k1_xonly_pubkey_save(secp256k1_xonly_pubkey *pubkey, secp256k1_ge *ge) {
#ifdef VERIFY
    /* ensure that the group element's Y coordinate is even, as per definition of x-only public keys */
    secp256k1_fe y = ge->y;
    secp256k1_fe_normalize_var(&y);
    VERIFY_CHECK(!secp256k1_fe_is_odd(&y));
#endif

    secp256k1_pubkey_save((secp256k1_pubkey *) pubkey, ge);
}

int secp256k1_xonly_pubkey_parse(const secp256k1_context* ctx, secp256k1_xonly_pubkey *pubkey, const unsigned char *input32) {
    secp256k1_ge pk;
    secp256k1_fe x;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(pubkey != NULL);
    memset(pubkey, 0, sizeof(*pubkey));
    ARG_CHECK(input32 != NULL);

    if (!secp256k1_fe_set_b32_limit(&x, input32)) {
        return 0;
    }
    if (!secp256k1_ge_set_xo_var(&pk, &x, 0)) {
        return 0;
    }
    if (!secp256k1_ge_is_in_correct_subgroup(&pk)) {
        return 0;
    }
    secp256k1_xonly_pubkey_save(pubkey, &pk);
    return 1;
}

int secp256k1_xonly_pubkey_serialize(const secp256k1_context* ctx, unsigned char *output32, const secp256k1_xonly_pubkey *pubkey) {
    secp256k1_ge pk;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(output32 != NULL);
    memset(output32, 0, 32);
    ARG_CHECK(pubkey != NULL);

    if (!secp256k1_xonly_pubkey_load(ctx, &pk, pubkey)) {
        return 0;
    }
    secp256k1_fe_get_b32(output32, &pk.x);
    return 1;
}

int secp256k1_xonly_pubkey_cmp(const secp256k1_context* ctx, const secp256k1_xonly_pubkey* pk0, const secp256k1_xonly_pubkey* pk1) {
    unsigned char out[2][32];
    const secp256k1_xonly_pubkey* pk[2];
    int i;

    VERIFY_CHECK(ctx != NULL);
    pk[0] = pk0; pk[1] = pk1;
    for (i = 0; i < 2; i++) {
        /* If the public key is NULL or invalid, xonly_pubkey_serialize will
         * call the illegal_callback and return 0. In that case we will
         * serialize the key as all zeros which is less than any valid public
         * key. This results in consistent comparisons even if NULL or invalid
         * pubkeys are involved and prevents edge cases such as sorting
         * algorithms that use this function and do not terminate as a
         * result. */
        if (!secp256k1_xonly_pubkey_serialize(ctx, out[i], pk[i])) {
            /* Note that xonly_pubkey_serialize should already set the output to
             * zero in that case, but it's not guaranteed by the API, we can't
             * test it and writing a VERIFY_CHECK is more complex than
             * explicitly memsetting (again). */
            memset(out[i], 0, sizeof(out[i]));
        }
    }
    return secp256k1_memcmp_var(out[0], out[1], sizeof(out[1]));
}

/** Keeps a group element as is if it has an even Y and otherwise negates it.
 *  y_parity is set to 0 in the former case and to 1 in the latter case.
 *  Requires that the coordinates of r are normalized. */
static int secp256k1_extrakeys_ge_even_y(secp256k1_ge *r) {
    int y_parity = 0;
    VERIFY_CHECK(!secp256k1_ge_is_infinity(r));

    if (secp256k1_fe_is_odd(&r->y)) {
        secp256k1_fe_negate(&r->y, &r->y, 1);
        y_parity = 1;
    }
    return y_parity;
}

int secp256k1_xonly_pubkey_from_pubkey(const secp256k1_context* ctx, secp256k1_xonly_pubkey *xonly_pubkey, int *pk_parity, const secp256k1_pubkey *pubkey) {
    secp256k1_ge pk;
    int tmp;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(xonly_pubkey != NULL);
    ARG_CHECK(pubkey != NULL);

    if (!secp256k1_pubkey_load(ctx, &pk, pubkey)) {
        return 0;
    }
    tmp = secp256k1_extrakeys_ge_even_y(&pk);
    if (pk_parity != NULL) {
        *pk_parity = tmp;
    }
    secp256k1_xonly_pubkey_save(xonly_pubkey, &pk);
    return 1;
}

int secp256k1_xonly_pubkey_tweak_add(const secp256k1_context* ctx, secp256k1_pubkey *output_pubkey, const secp256k1_xonly_pubkey *internal_pubkey, const unsigned char *tweak32) {
    secp256k1_ge pk;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(output_pubkey != NULL);
    memset(output_pubkey, 0, sizeof(*output_pubkey));
    ARG_CHECK(internal_pubkey != NULL);
    ARG_CHECK(tweak32 != NULL);

    if (!secp256k1_xonly_pubkey_load(ctx, &pk, internal_pubkey)
        || !secp256k1_ec_pubkey_tweak_add_helper(&pk, tweak32)) {
        return 0;
    }
    secp256k1_pubkey_save(output_pubkey, &pk);
    return 1;
}

int secp256k1_xonly_pubkey_tweak_add_check(const secp256k1_context* ctx, const unsigned char *tweaked_pubkey32, int tweaked_pk_parity, const secp256k1_xonly_pubkey *internal_pubkey, const unsigned char *tweak32) {
    secp256k1_ge pk;
    unsigned char pk_expected32[32];

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(internal_pubkey != NULL);
    ARG_CHECK(tweaked_pubkey32 != NULL);
    ARG_CHECK(tweak32 != NULL);

    if (!secp256k1_xonly_pubkey_load(ctx, &pk, internal_pubkey)
        || !secp256k1_ec_pubkey_tweak_add_helper(&pk, tweak32)) {
        return 0;
    }
    secp256k1_fe_normalize_var(&pk.x);
    secp256k1_fe_normalize_var(&pk.y);
    secp256k1_fe_get_b32(pk_expected32, &pk.x);

    return secp256k1_memcmp_var(&pk_expected32, tweaked_pubkey32, 32) == 0
            && secp256k1_fe_is_odd(&pk.y) == tweaked_pk_parity;
}

#ifdef SECP256K1_ECDSA_VERIFY_MANY_IFMA
/* Parse count (at most eight) keys, with the square roots computed in the IFMA lanes. */
static void secp256k1_xonly_pubkey_ifma_parse8(unsigned char *results, secp256k1_xonly_pubkey *pubkeys, const unsigned char *inputs32, size_t count) {
    static const int even[SECP256K1_ECDSA_IFMA_LANES] = {0};
    secp256k1_fe x[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_ge pk[SECP256K1_ECDSA_IFMA_LANES];
    int ok[SECP256K1_ECDSA_IFMA_LANES];
    int on_curve[SECP256K1_ECDSA_IFMA_LANES];
    size_t lane;

    for (lane = 0; lane < SECP256K1_ECDSA_IFMA_LANES; lane++) {
        ok[lane] = secp256k1_fe_set_b32_limit(&x[lane], &inputs32[32 * (lane < count ? lane : 0)]);
        if (!ok[lane]) {
            secp256k1_fe_set_int(&x[lane], 1);
        }
    }
    secp256k1_ecdsa_ifma_ge_set_xo8_var(pk, on_curve, x, even);
    for (lane = 0; lane < count; lane++) {
        results[lane] = ok[lane] && on_curve[lane] && secp256k1_ge_is_in_correct_subgroup(&pk[lane]);
        if (results[lane]) {
            secp256k1_xonly_pubkey_save(&pubkeys[lane], &pk[lane]);
        } else {
            memset(&pubkeys[lane], 0, sizeof(pubkeys[lane]));
        }
    }
}
#endif

int secp256k1_xonly_pubkey_parse_many(const secp256k1_context* ctx, unsigned char *results, secp256k1_xonly_pubkey *pubkeys, const unsigned char *inputs32, size_t n) {
    size_t i;
    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(results != NULL);
    ARG_CHECK(pubkeys != NULL);
    ARG_CHECK(inputs32 != NULL);
    ARG_CHECK(n > 0);

#ifdef SECP256K1_ECDSA_VERIFY_MANY_IFMA
    if (n >= SECP256K1_ECDSA_IFMA_LANES && secp256k1_ecdsa_ifma_available()) {
        for (i = 0; i < n; i += SECP256K1_ECDSA_IFMA_LANES) {
            size_t count = n - i < SECP256K1_ECDSA_IFMA_LANES ? n - i : SECP256K1_ECDSA_IFMA_LANES;
            secp256k1_xonly_pubkey_ifma_parse8(&results[i], &pubkeys[i], &inputs32[32 * i], count);
        }
        return 1;
    }
#endif
    for (i = 0; i < n; i++) {
        results[i] = (unsigned char)secp256k1_xonly_pubkey_parse(ctx, &pubkeys[i], &inputs32[32 * i]);
    }
    return 1;
}

#ifdef SECP256K1_ECDSA_VERIFY_MANY_IFMA
/* Check count (at most eight) tweaked keys in the IFMA lanes, computing
 * internal + tweak*G as 1*internal + tweak*G. A lane at infinity, from an
 * exceptional addition or an infinite sum, is checked again with
 * secp256k1_xonly_pubkey_tweak_add_check; any other result is exact. */
SECP256K1_ECDSA_IFMA_TARGET static void secp256k1_xonly_pubkey_ifma_tweak_add_check8(const secp256k1_context *ctx, unsigned char *results, const unsigned char *tweaked_pubkeys32, const int *tweaked_pk_parities, const secp256k1_xonly_pubkey *internal_pubkeys, const unsigned char *tweaks32, size_t count) {
    secp256k1_ecdsa_ifma_prepared8 p;
    secp256k1_scalar tweak[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_scalar one[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_ge internal[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_ge sum[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_fe expected_x[SECP256K1_ECDSA_IFMA_LANES];
    int eligible[SECP256K1_ECDSA_IFMA_LANES];
    size_t lane;

    memset(&p, 0, sizeof(p));
    for (lane = 0; lane < SECP256K1_ECDSA_IFMA_LANES; lane++) {
        size_t at = lane < count ? lane : 0;
        int overflow;
        secp256k1_scalar_set_int(&one[lane], 1);
        secp256k1_scalar_set_b32(&tweak[lane], &tweaks32[32 * at], &overflow);
        eligible[lane] = !overflow && secp256k1_fe_set_b32_limit(&expected_x[lane], &tweaked_pubkeys32[32 * at]);
        if (!secp256k1_xonly_pubkey_load(ctx, &internal[lane], &internal_pubkeys[at])) {
            eligible[lane] = 0;
            internal[lane] = secp256k1_ge_const_g;
        }
    }
    secp256k1_ecdsa_ifma_prepare_mult8(&p, tweak, one, internal);
    secp256k1_ecdsa_ifma_to_affine8(sum, &p, secp256k1_ecdsa_ifma_mult8(&p));
    for (lane = 0; lane < count; lane++) {
        if (!eligible[lane]) {
            results[lane] = 0;
        } else if (secp256k1_ge_is_infinity(&sum[lane])) {
            results[lane] = (unsigned char)secp256k1_xonly_pubkey_tweak_add_check(ctx, &tweaked_pubkeys32[32 * lane], tweaked_pk_parities[lane], &internal_pubkeys[lane], &tweaks32[32 * lane]);
        } else {
            secp256k1_fe_normalize_var(&sum[lane].y);
            results[lane] = (unsigned char)(secp256k1_fe_equal(&expected_x[lane], &sum[lane].x) && secp256k1_fe_is_odd(&sum[lane].y) == tweaked_pk_parities[lane]);
        }
    }
}
#endif

int secp256k1_xonly_pubkey_tweak_add_check_many(const secp256k1_context* ctx, unsigned char *results, const unsigned char *tweaked_pubkeys32, const int *tweaked_pk_parities, const secp256k1_xonly_pubkey *internal_pubkeys, const unsigned char *tweaks32, size_t n) {
    size_t i;
    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(results != NULL);
    ARG_CHECK(tweaked_pubkeys32 != NULL);
    ARG_CHECK(tweaked_pk_parities != NULL);
    ARG_CHECK(internal_pubkeys != NULL);
    ARG_CHECK(tweaks32 != NULL);
    ARG_CHECK(n > 0);

#ifdef SECP256K1_ECDSA_VERIFY_MANY_IFMA
    if (n >= SECP256K1_ECDSA_IFMA_LANES && secp256k1_ecdsa_ifma_available()) {
        for (i = 0; i < n; i += SECP256K1_ECDSA_IFMA_LANES) {
            size_t count = n - i < SECP256K1_ECDSA_IFMA_LANES ? n - i : SECP256K1_ECDSA_IFMA_LANES;
            secp256k1_xonly_pubkey_ifma_tweak_add_check8(ctx, &results[i], &tweaked_pubkeys32[32 * i], &tweaked_pk_parities[i], &internal_pubkeys[i], &tweaks32[32 * i], count);
        }
        return 1;
    }
#endif
    for (i = 0; i < n; i++) {
        results[i] = (unsigned char)secp256k1_xonly_pubkey_tweak_add_check(ctx, &tweaked_pubkeys32[32 * i], tweaked_pk_parities[i], &internal_pubkeys[i], &tweaks32[32 * i]);
    }
    return 1;
}

static void secp256k1_keypair_save(secp256k1_keypair *keypair, const secp256k1_scalar *sk, secp256k1_ge *pk) {
    secp256k1_scalar_get_b32(&keypair->data[0], sk);
    secp256k1_pubkey_save((secp256k1_pubkey *)&keypair->data[32], pk);
}


static int secp256k1_keypair_seckey_load(const secp256k1_context* ctx, secp256k1_scalar *sk, const secp256k1_keypair *keypair) {
    int ret;

    ret = secp256k1_scalar_set_b32_seckey(sk, &keypair->data[0]);
    /* We can declassify ret here because sk is only zero if a keypair function
     * failed (which zeroes the keypair) and its return value is ignored. */
    secp256k1_declassify(ctx, &ret, sizeof(ret));
    ARG_CHECK(ret);
    return ret;
}

/* Load a keypair into pk and sk (if non-NULL). This function declassifies pk
 * and ARG_CHECKs that the keypair is not invalid. It always initializes sk and
 * pk with dummy values. */
static int secp256k1_keypair_load(const secp256k1_context* ctx, secp256k1_scalar *sk, secp256k1_ge *pk, const secp256k1_keypair *keypair) {
    int ret;
    const secp256k1_pubkey *pubkey = (const secp256k1_pubkey *)&keypair->data[32];

    /* Need to declassify the pubkey because pubkey_load ARG_CHECKs if it's
     * invalid. */
    secp256k1_declassify(ctx, pubkey, sizeof(*pubkey));
    ret = secp256k1_pubkey_load(ctx, pk, pubkey);
    if (sk != NULL) {
        ret = ret && secp256k1_keypair_seckey_load(ctx, sk, keypair);
    }
    if (!ret) {
        *pk = secp256k1_ge_const_g;
        if (sk != NULL) {
            *sk = secp256k1_scalar_one;
        }
    }
    return ret;
}

int secp256k1_keypair_create(const secp256k1_context* ctx, secp256k1_keypair *keypair, const unsigned char *seckey32) {
    secp256k1_scalar sk;
    secp256k1_ge pk;
    int ret = 0;
    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(keypair != NULL);
    memset(keypair, 0, sizeof(*keypair));
    ARG_CHECK(secp256k1_ecmult_gen_context_is_built(&ctx->ecmult_gen_ctx));
    ARG_CHECK(seckey32 != NULL);

    ret = secp256k1_ec_pubkey_create_helper(&ctx->ecmult_gen_ctx, &sk, &pk, seckey32);
    secp256k1_keypair_save(keypair, &sk, &pk);
    secp256k1_memczero(keypair, sizeof(*keypair), !ret);

    secp256k1_scalar_clear(&sk);
    return ret;
}

int secp256k1_keypair_sec(const secp256k1_context* ctx, unsigned char *seckey, const secp256k1_keypair *keypair) {
    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(seckey != NULL);
    memset(seckey, 0, 32);
    ARG_CHECK(keypair != NULL);

    memcpy(seckey, &keypair->data[0], 32);
    return 1;
}

int secp256k1_keypair_pub(const secp256k1_context* ctx, secp256k1_pubkey *pubkey, const secp256k1_keypair *keypair) {
    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(pubkey != NULL);
    memset(pubkey, 0, sizeof(*pubkey));
    ARG_CHECK(keypair != NULL);

    memcpy(pubkey->data, &keypair->data[32], sizeof(*pubkey));
    return 1;
}

int secp256k1_keypair_xonly_pub(const secp256k1_context* ctx, secp256k1_xonly_pubkey *pubkey, int *pk_parity, const secp256k1_keypair *keypair) {
    secp256k1_ge pk;
    int tmp;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(pubkey != NULL);
    memset(pubkey, 0, sizeof(*pubkey));
    ARG_CHECK(keypair != NULL);

    if (!secp256k1_keypair_load(ctx, NULL, &pk, keypair)) {
        return 0;
    }
    tmp = secp256k1_extrakeys_ge_even_y(&pk);
    if (pk_parity != NULL) {
        *pk_parity = tmp;
    }
    secp256k1_xonly_pubkey_save(pubkey, &pk);

    return 1;
}

int secp256k1_keypair_xonly_tweak_add(const secp256k1_context* ctx, secp256k1_keypair *keypair, const unsigned char *tweak32) {
    secp256k1_ge pk;
    secp256k1_scalar sk;
    int y_parity;
    int ret;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(keypair != NULL);
    ARG_CHECK(tweak32 != NULL);

    ret = secp256k1_keypair_load(ctx, &sk, &pk, keypair);
    memset(keypair, 0, sizeof(*keypair));

    y_parity = secp256k1_extrakeys_ge_even_y(&pk);
    if (y_parity == 1) {
        secp256k1_scalar_negate(&sk, &sk);
    }

    ret &= secp256k1_ec_seckey_tweak_add_helper(&sk, tweak32);
    ret &= secp256k1_ec_pubkey_tweak_add_helper(&pk, tweak32);

    secp256k1_declassify(ctx, &ret, sizeof(ret));
    if (ret) {
        secp256k1_keypair_save(keypair, &sk, &pk);
    }

    secp256k1_scalar_clear(&sk);
    return ret;
}

#endif
