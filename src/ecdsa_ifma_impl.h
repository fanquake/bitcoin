/***********************************************************************
 * AVX-512 IFMA multi-buffer ECDSA verification.                       *
 * Distributed under the MIT software license, see the accompanying    *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.*
 ***********************************************************************/

#ifndef SECP256K1_ECDSA_IFMA_IMPL_H
#define SECP256K1_ECDSA_IFMA_IMPL_H

/* Verification inputs are public, so digit lookups and the checked
 * exceptional-case fallback are permitted to be variable-time. */

#include <string.h>

#include "ecdsa_ifma.h"
#include "precomputed_ecdsa_ifma.h"

#if !defined(__x86_64__)
#error "AVX-512 IFMA ECDSA verification requires x86-64"
#endif

#if !defined(SECP256K1_WIDEMUL_INT128)
#error "AVX-512 IFMA ECDSA verification requires the 5x52 field implementation"
#endif

#define SECP256K1_ECDSA_IFMA_INLINE static SECP256K1_ECDSA_IFMA_TARGET SECP256K1_FORCE_INLINE
#define SECP256K1_ECDSA_IFMA_ALIGN64 __attribute__((aligned(64)))
#define SECP256K1_ECDSA_IFMA_M52 UINT64_C(0xFFFFFFFFFFFFF)
#define SECP256K1_ECDSA_IFMA_RP UINT64_C(0x1000003D10)
#define SECP256K1_ECDSA_IFMA_C256 UINT64_C(0x1000003D1)

SECP256K1_ECDSA_IFMA_INLINE __m512i secp256k1_ecdsa_ifma_vzero(void) {
    return _mm512_setzero_si512();
}

SECP256K1_ECDSA_IFMA_INLINE __m512i secp256k1_ecdsa_ifma_vsplat(uint64_t x) {
    return _mm512_set1_epi64((long long)x);
}

#define SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r, a) do { \
    v = _mm512_add_epi64((a), c); \
    (r) = _mm512_and_si512(v, secp256k1_ecdsa_ifma_vsplat(SECP256K1_ECDSA_IFMA_M52)); \
    c = _mm512_srli_epi64(v, 52); \
} while(0)

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_zero(void) {
    secp256k1_ecdsa_ifma_fe8 r;
    r.n[0] = secp256k1_ecdsa_ifma_vzero();
    r.n[1] = secp256k1_ecdsa_ifma_vzero();
    r.n[2] = secp256k1_ecdsa_ifma_vzero();
    r.n[3] = secp256k1_ecdsa_ifma_vzero();
    r.n[4] = secp256k1_ecdsa_ifma_vzero();
    return r;
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_one(void) {
    secp256k1_ecdsa_ifma_fe8 r = secp256k1_ecdsa_ifma_fe8_zero();
    r.n[0] = secp256k1_ecdsa_ifma_vsplat(1);
    return r;
}

/* Fold the bits above 2^256 into the low limb and carry through limbs 1 to 3, so
 * that every limb is below 2^52 (limb 4 at most 2^48). The multiplication
 * instructions only read the low 52 bits of each limb, so a wider limb would
 * silently lose its top bit. */
SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_top_fold(secp256k1_ecdsa_ifma_fe8 r) {
    const __m512i m52 = secp256k1_ecdsa_ifma_vsplat(SECP256K1_ECDSA_IFMA_M52);
    __m512i top = _mm512_srli_epi64(r.n[4], 48);
    __m512i v;
    r.n[4] = _mm512_and_si512(r.n[4], secp256k1_ecdsa_ifma_vsplat(UINT64_C(0xFFFFFFFFFFFF)));
    v = _mm512_madd52lo_epu64(r.n[0], top, secp256k1_ecdsa_ifma_vsplat(SECP256K1_ECDSA_IFMA_C256));
    r.n[0] = _mm512_and_si512(v, m52);
    r.n[1] = _mm512_add_epi64(r.n[1], _mm512_srli_epi64(v, 52));
    r.n[2] = _mm512_add_epi64(r.n[2], _mm512_srli_epi64(r.n[1], 52));
    r.n[1] = _mm512_and_si512(r.n[1], m52);
    r.n[3] = _mm512_add_epi64(r.n[3], _mm512_srli_epi64(r.n[2], 52));
    r.n[2] = _mm512_and_si512(r.n[2], m52);
    r.n[4] = _mm512_add_epi64(r.n[4], _mm512_srli_epi64(r.n[3], 52));
    r.n[3] = _mm512_and_si512(r.n[3], m52);
    return r;
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_norm5(__m512i t0, __m512i t1, __m512i t2, __m512i t3, __m512i t4) {
    secp256k1_ecdsa_ifma_fe8 r;
    __m512i c = secp256k1_ecdsa_ifma_vzero();
    __m512i v;
    __m512i folded;
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[0], t0);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[1], t1);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[2], t2);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[3], t3);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[4], t4);
    folded = _mm512_add_epi64(r.n[0], _mm512_mullo_epi64(c, secp256k1_ecdsa_ifma_vsplat(SECP256K1_ECDSA_IFMA_RP)));
    r.n[0] = _mm512_and_si512(folded, secp256k1_ecdsa_ifma_vsplat(SECP256K1_ECDSA_IFMA_M52));
    r.n[1] = _mm512_add_epi64(r.n[1], _mm512_srli_epi64(folded, 52));
    return secp256k1_ecdsa_ifma_fe8_top_fold(r);
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_add(secp256k1_ecdsa_ifma_fe8 a, secp256k1_ecdsa_ifma_fe8 b) {
    return secp256k1_ecdsa_ifma_fe8_norm5(
        _mm512_add_epi64(a.n[0], b.n[0]),
        _mm512_add_epi64(a.n[1], b.n[1]),
        _mm512_add_epi64(a.n[2], b.n[2]),
        _mm512_add_epi64(a.n[3], b.n[3]),
        _mm512_add_epi64(a.n[4], b.n[4])
    );
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_sub(secp256k1_ecdsa_ifma_fe8 a, secp256k1_ecdsa_ifma_fe8 b) {
    const uint64_t p0 = UINT64_C(0xFFFFEFFFFFC2F);
    const uint64_t pm = UINT64_C(0xFFFFFFFFFFFFF);
    const uint64_t p4 = UINT64_C(0xFFFFFFFFFFFF);
    return secp256k1_ecdsa_ifma_fe8_norm5(
        _mm512_sub_epi64(_mm512_add_epi64(a.n[0], secp256k1_ecdsa_ifma_vsplat(2 * p0)), b.n[0]),
        _mm512_sub_epi64(_mm512_add_epi64(a.n[1], secp256k1_ecdsa_ifma_vsplat(2 * pm)), b.n[1]),
        _mm512_sub_epi64(_mm512_add_epi64(a.n[2], secp256k1_ecdsa_ifma_vsplat(2 * pm)), b.n[2]),
        _mm512_sub_epi64(_mm512_add_epi64(a.n[3], secp256k1_ecdsa_ifma_vsplat(2 * pm)), b.n[3]),
        _mm512_sub_epi64(_mm512_add_epi64(a.n[4], secp256k1_ecdsa_ifma_vsplat(2 * p4)), b.n[4])
    );
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_mul_int(secp256k1_ecdsa_ifma_fe8 a, uint64_t k) {
    __m512i kv = secp256k1_ecdsa_ifma_vsplat(k);
    return secp256k1_ecdsa_ifma_fe8_norm5(
        _mm512_mullo_epi64(a.n[0], kv),
        _mm512_mullo_epi64(a.n[1], kv),
        _mm512_mullo_epi64(a.n[2], kv),
        _mm512_mullo_epi64(a.n[3], kv),
        _mm512_mullo_epi64(a.n[4], kv)
    );
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_neg(secp256k1_ecdsa_ifma_fe8 a) {
    return secp256k1_ecdsa_ifma_fe8_sub(secp256k1_ecdsa_ifma_fe8_zero(), a);
}

#define SECP256K1_ECDSA_IFMA_M52LO(acc, a, b) _mm512_madd52lo_epu64((acc), (a), (b))
#define SECP256K1_ECDSA_IFMA_M52HI(acc, a, b) _mm512_madd52hi_epu64((acc), (a), (b))
#define SECP256K1_ECDSA_IFMA_MUL_TERM(i, j) do { \
    t[(i) + (j)] = SECP256K1_ECDSA_IFMA_M52LO(t[(i) + (j)], a.n[(i)], b.n[(j)]); \
    t[(i) + (j) + 1] = SECP256K1_ECDSA_IFMA_M52HI(t[(i) + (j) + 1], a.n[(i)], b.n[(j)]); \
} while(0)
#define SECP256K1_ECDSA_IFMA_MUL_ROW(i) do { \
    SECP256K1_ECDSA_IFMA_MUL_TERM(i, 0); \
    SECP256K1_ECDSA_IFMA_MUL_TERM(i, 1); \
    SECP256K1_ECDSA_IFMA_MUL_TERM(i, 2); \
    SECP256K1_ECDSA_IFMA_MUL_TERM(i, 3); \
    SECP256K1_ECDSA_IFMA_MUL_TERM(i, 4); \
} while(0)

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_reduce_product(__m512i t[10]) {
    secp256k1_ecdsa_ifma_fe8 r;
    __m512i h[5];
    __m512i c = secp256k1_ecdsa_ifma_vzero();
    __m512i v;
    __m512i c9;
    __m512i rp = secp256k1_ecdsa_ifma_vsplat(SECP256K1_ECDSA_IFMA_RP);
    __m512i k0;
    __m512i k0h;
    __m512i res5;
    __m512i f;
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(h[0], t[5]);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(h[1], t[6]);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(h[2], t[7]);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(h[3], t[8]);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(h[4], t[9]);
    c9 = c;
    t[0] = SECP256K1_ECDSA_IFMA_M52LO(t[0], h[0], rp);
    t[1] = SECP256K1_ECDSA_IFMA_M52HI(t[1], h[0], rp);
    t[1] = SECP256K1_ECDSA_IFMA_M52LO(t[1], h[1], rp);
    t[2] = SECP256K1_ECDSA_IFMA_M52HI(t[2], h[1], rp);
    t[2] = SECP256K1_ECDSA_IFMA_M52LO(t[2], h[2], rp);
    t[3] = SECP256K1_ECDSA_IFMA_M52HI(t[3], h[2], rp);
    t[3] = SECP256K1_ECDSA_IFMA_M52LO(t[3], h[3], rp);
    t[4] = SECP256K1_ECDSA_IFMA_M52HI(t[4], h[3], rp);
    t[4] = SECP256K1_ECDSA_IFMA_M52LO(t[4], h[4], rp);
    res5 = SECP256K1_ECDSA_IFMA_M52HI(secp256k1_ecdsa_ifma_vzero(), h[4], rp);
    t[0] = SECP256K1_ECDSA_IFMA_M52LO(t[0], res5, rp);
    t[1] = SECP256K1_ECDSA_IFMA_M52HI(t[1], res5, rp);
    k0 = SECP256K1_ECDSA_IFMA_M52LO(secp256k1_ecdsa_ifma_vzero(), c9, rp);
    k0h = SECP256K1_ECDSA_IFMA_M52HI(secp256k1_ecdsa_ifma_vzero(), c9, rp);
    t[0] = SECP256K1_ECDSA_IFMA_M52LO(t[0], k0, rp);
    t[1] = SECP256K1_ECDSA_IFMA_M52HI(t[1], k0, rp);
    t[1] = _mm512_add_epi64(t[1], k0h);
    c = secp256k1_ecdsa_ifma_vzero();
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[0], t[0]);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[1], t[1]);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[2], t[2]);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[3], t[3]);
    SECP256K1_ECDSA_IFMA_EXTRACT_LIMB(r.n[4], t[4]);
    f = SECP256K1_ECDSA_IFMA_M52LO(secp256k1_ecdsa_ifma_vzero(), c, rp);
    v = _mm512_add_epi64(r.n[0], f);
    r.n[0] = _mm512_and_si512(v, secp256k1_ecdsa_ifma_vsplat(SECP256K1_ECDSA_IFMA_M52));
    r.n[1] = _mm512_add_epi64(r.n[1], _mm512_srli_epi64(v, 52));
    return secp256k1_ecdsa_ifma_fe8_top_fold(r);
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_mul(secp256k1_ecdsa_ifma_fe8 a, secp256k1_ecdsa_ifma_fe8 b) {
    __m512i t[10];
    memset(t, 0, sizeof(t));
    SECP256K1_ECDSA_IFMA_MUL_ROW(0);
    SECP256K1_ECDSA_IFMA_MUL_ROW(1);
    SECP256K1_ECDSA_IFMA_MUL_ROW(2);
    SECP256K1_ECDSA_IFMA_MUL_ROW(3);
    SECP256K1_ECDSA_IFMA_MUL_ROW(4);
    return secp256k1_ecdsa_ifma_fe8_reduce_product(t);
}

#undef SECP256K1_ECDSA_IFMA_MUL_ROW
#undef SECP256K1_ECDSA_IFMA_MUL_TERM

#define SECP256K1_ECDSA_IFMA_SQR_CROSS(i, j) do { \
    t[(i) + (j)] = SECP256K1_ECDSA_IFMA_M52LO(t[(i) + (j)], a.n[(i)], a.n[(j)]); \
    t[(i) + (j) + 1] = SECP256K1_ECDSA_IFMA_M52HI(t[(i) + (j) + 1], a.n[(i)], a.n[(j)]); \
} while(0)
#define SECP256K1_ECDSA_IFMA_SQR_DIAG(i) do { \
    t[2 * (i)] = SECP256K1_ECDSA_IFMA_M52LO(t[2 * (i)], a.n[(i)], a.n[(i)]); \
    t[2 * (i) + 1] = SECP256K1_ECDSA_IFMA_M52HI(t[2 * (i) + 1], a.n[(i)], a.n[(i)]); \
} while(0)

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_sqr(secp256k1_ecdsa_ifma_fe8 a) {
    __m512i t[10];
    memset(t, 0, sizeof(t));
    SECP256K1_ECDSA_IFMA_SQR_CROSS(0, 1);
    SECP256K1_ECDSA_IFMA_SQR_CROSS(0, 2);
    SECP256K1_ECDSA_IFMA_SQR_CROSS(0, 3);
    SECP256K1_ECDSA_IFMA_SQR_CROSS(0, 4);
    SECP256K1_ECDSA_IFMA_SQR_CROSS(1, 2);
    SECP256K1_ECDSA_IFMA_SQR_CROSS(1, 3);
    SECP256K1_ECDSA_IFMA_SQR_CROSS(1, 4);
    SECP256K1_ECDSA_IFMA_SQR_CROSS(2, 3);
    SECP256K1_ECDSA_IFMA_SQR_CROSS(2, 4);
    SECP256K1_ECDSA_IFMA_SQR_CROSS(3, 4);
    t[0] = _mm512_add_epi64(t[0], t[0]);
    t[1] = _mm512_add_epi64(t[1], t[1]);
    t[2] = _mm512_add_epi64(t[2], t[2]);
    t[3] = _mm512_add_epi64(t[3], t[3]);
    t[4] = _mm512_add_epi64(t[4], t[4]);
    t[5] = _mm512_add_epi64(t[5], t[5]);
    t[6] = _mm512_add_epi64(t[6], t[6]);
    t[7] = _mm512_add_epi64(t[7], t[7]);
    t[8] = _mm512_add_epi64(t[8], t[8]);
    t[9] = _mm512_add_epi64(t[9], t[9]);
    SECP256K1_ECDSA_IFMA_SQR_DIAG(0);
    SECP256K1_ECDSA_IFMA_SQR_DIAG(1);
    SECP256K1_ECDSA_IFMA_SQR_DIAG(2);
    SECP256K1_ECDSA_IFMA_SQR_DIAG(3);
    SECP256K1_ECDSA_IFMA_SQR_DIAG(4);
    return secp256k1_ecdsa_ifma_fe8_reduce_product(t);
}

#undef SECP256K1_ECDSA_IFMA_SQR_CROSS
#undef SECP256K1_ECDSA_IFMA_SQR_DIAG
#undef SECP256K1_ECDSA_IFMA_M52LO
#undef SECP256K1_ECDSA_IFMA_M52HI
#undef SECP256K1_ECDSA_IFMA_EXTRACT_LIMB

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_select(__mmask8 mask, secp256k1_ecdsa_ifma_fe8 t, secp256k1_ecdsa_ifma_fe8 f) {
    secp256k1_ecdsa_ifma_fe8 r;
    r.n[0] = _mm512_mask_blend_epi64(mask, f.n[0], t.n[0]);
    r.n[1] = _mm512_mask_blend_epi64(mask, f.n[1], t.n[1]);
    r.n[2] = _mm512_mask_blend_epi64(mask, f.n[2], t.n[2]);
    r.n[3] = _mm512_mask_blend_epi64(mask, f.n[3], t.n[3]);
    r.n[4] = _mm512_mask_blend_epi64(mask, f.n[4], t.n[4]);
    return r;
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_jac8 secp256k1_ecdsa_ifma_jac8_select(__mmask8 mask, secp256k1_ecdsa_ifma_jac8 t, secp256k1_ecdsa_ifma_jac8 f) {
    secp256k1_ecdsa_ifma_jac8 r;
    r.x = secp256k1_ecdsa_ifma_fe8_select(mask, t.x, f.x);
    r.y = secp256k1_ecdsa_ifma_fe8_select(mask, t.y, f.y);
    r.z = secp256k1_ecdsa_ifma_fe8_select(mask, t.z, f.z);
    return r;
}

SECP256K1_ECDSA_IFMA_TARGET static secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_pack(const secp256k1_fe a[SECP256K1_ECDSA_IFMA_LANES]) {
    secp256k1_ecdsa_ifma_fe8 r;
#define SECP256K1_ECDSA_IFMA_PACK_LIMB(k) _mm512_set_epi64( \
    (long long)a[7].n[(k)], (long long)a[6].n[(k)], \
    (long long)a[5].n[(k)], (long long)a[4].n[(k)], \
    (long long)a[3].n[(k)], (long long)a[2].n[(k)], \
    (long long)a[1].n[(k)], (long long)a[0].n[(k)] \
)
    r.n[0] = SECP256K1_ECDSA_IFMA_PACK_LIMB(0);
    r.n[1] = SECP256K1_ECDSA_IFMA_PACK_LIMB(1);
    r.n[2] = SECP256K1_ECDSA_IFMA_PACK_LIMB(2);
    r.n[3] = SECP256K1_ECDSA_IFMA_PACK_LIMB(3);
    r.n[4] = SECP256K1_ECDSA_IFMA_PACK_LIMB(4);
#undef SECP256K1_ECDSA_IFMA_PACK_LIMB
    return r;
}

SECP256K1_ECDSA_IFMA_TARGET static secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_splat_fe(const secp256k1_fe *a) {
    secp256k1_ecdsa_ifma_fe8 r;
    r.n[0] = secp256k1_ecdsa_ifma_vsplat(a->n[0]);
    r.n[1] = secp256k1_ecdsa_ifma_vsplat(a->n[1]);
    r.n[2] = secp256k1_ecdsa_ifma_vsplat(a->n[2]);
    r.n[3] = secp256k1_ecdsa_ifma_vsplat(a->n[3]);
    r.n[4] = secp256k1_ecdsa_ifma_vsplat(a->n[4]);
    return r;
}

SECP256K1_ECDSA_IFMA_TARGET static secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_fe8_splat_limbs(const uint64_t a[5]) {
    secp256k1_ecdsa_ifma_fe8 r;
    r.n[0] = secp256k1_ecdsa_ifma_vsplat(a[0]);
    r.n[1] = secp256k1_ecdsa_ifma_vsplat(a[1]);
    r.n[2] = secp256k1_ecdsa_ifma_vsplat(a[2]);
    r.n[3] = secp256k1_ecdsa_ifma_vsplat(a[3]);
    r.n[4] = secp256k1_ecdsa_ifma_vsplat(a[4]);
    return r;
}

SECP256K1_ECDSA_IFMA_TARGET static void secp256k1_ecdsa_ifma_fe8_get_lane(secp256k1_fe *r, const secp256k1_ecdsa_ifma_fe8 *a, int lane) {
    uint64_t limbs[SECP256K1_ECDSA_IFMA_LANES] SECP256K1_ECDSA_IFMA_ALIGN64;
    _mm512_store_si512((void *)limbs, a->n[0]);
    r->n[0] = limbs[lane];
    _mm512_store_si512((void *)limbs, a->n[1]);
    r->n[1] = limbs[lane];
    _mm512_store_si512((void *)limbs, a->n[2]);
    r->n[2] = limbs[lane];
    _mm512_store_si512((void *)limbs, a->n[3]);
    r->n[3] = limbs[lane];
    _mm512_store_si512((void *)limbs, a->n[4]);
    r->n[4] = limbs[lane];
#ifdef VERIFY
    r->magnitude = 32;
    r->normalized = 0;
#endif
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_jac8 secp256k1_ecdsa_ifma_point_double(secp256k1_ecdsa_ifma_jac8 p) {
    secp256k1_ecdsa_ifma_fe8 a = secp256k1_ecdsa_ifma_fe8_sqr(p.x);
    secp256k1_ecdsa_ifma_fe8 b = secp256k1_ecdsa_ifma_fe8_sqr(p.y);
    secp256k1_ecdsa_ifma_fe8 c = secp256k1_ecdsa_ifma_fe8_sqr(b);
    secp256k1_ecdsa_ifma_fe8 xb = secp256k1_ecdsa_ifma_fe8_add(p.x, b);
    secp256k1_ecdsa_ifma_fe8 d = secp256k1_ecdsa_ifma_fe8_mul_int(secp256k1_ecdsa_ifma_fe8_sub(secp256k1_ecdsa_ifma_fe8_sub(secp256k1_ecdsa_ifma_fe8_sqr(xb), a), c), 2);
    secp256k1_ecdsa_ifma_fe8 e = secp256k1_ecdsa_ifma_fe8_mul_int(a, 3);
    secp256k1_ecdsa_ifma_fe8 f = secp256k1_ecdsa_ifma_fe8_sqr(e);
    secp256k1_ecdsa_ifma_jac8 r;
    r.x = secp256k1_ecdsa_ifma_fe8_sub(f, secp256k1_ecdsa_ifma_fe8_mul_int(d, 2));
    r.y = secp256k1_ecdsa_ifma_fe8_sub(secp256k1_ecdsa_ifma_fe8_mul(e, secp256k1_ecdsa_ifma_fe8_sub(d, r.x)), secp256k1_ecdsa_ifma_fe8_mul_int(c, 8));
    r.z = secp256k1_ecdsa_ifma_fe8_mul_int(secp256k1_ecdsa_ifma_fe8_mul(p.y, p.z), 2);
    return r;
}

typedef struct {
    secp256k1_ecdsa_ifma_jac8 p;
    secp256k1_ecdsa_ifma_fe8 z_ratio;
} secp256k1_ecdsa_ifma_add_ratio8;

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_add_ratio8 secp256k1_ecdsa_ifma_point_add_affine_ratio(secp256k1_ecdsa_ifma_jac8 p, secp256k1_ecdsa_ifma_aff8 q) {
    secp256k1_ecdsa_ifma_fe8 z1z1 = secp256k1_ecdsa_ifma_fe8_sqr(p.z);
    secp256k1_ecdsa_ifma_fe8 u2 = secp256k1_ecdsa_ifma_fe8_mul(q.x, z1z1);
    secp256k1_ecdsa_ifma_fe8 s2 = secp256k1_ecdsa_ifma_fe8_mul(q.y, secp256k1_ecdsa_ifma_fe8_mul(p.z, z1z1));
    secp256k1_ecdsa_ifma_fe8 h = secp256k1_ecdsa_ifma_fe8_sub(u2, p.x);
    secp256k1_ecdsa_ifma_fe8 hh = secp256k1_ecdsa_ifma_fe8_sqr(h);
    secp256k1_ecdsa_ifma_fe8 i = secp256k1_ecdsa_ifma_fe8_mul_int(hh, 4);
    secp256k1_ecdsa_ifma_fe8 j = secp256k1_ecdsa_ifma_fe8_mul(h, i);
    secp256k1_ecdsa_ifma_fe8 rr = secp256k1_ecdsa_ifma_fe8_mul_int(secp256k1_ecdsa_ifma_fe8_sub(s2, p.y), 2);
    secp256k1_ecdsa_ifma_fe8 v = secp256k1_ecdsa_ifma_fe8_mul(p.x, i);
    secp256k1_ecdsa_ifma_add_ratio8 out;
    out.p.x = secp256k1_ecdsa_ifma_fe8_sub(secp256k1_ecdsa_ifma_fe8_sub(secp256k1_ecdsa_ifma_fe8_sqr(rr), j), secp256k1_ecdsa_ifma_fe8_mul_int(v, 2));
    out.p.y = secp256k1_ecdsa_ifma_fe8_sub(secp256k1_ecdsa_ifma_fe8_mul(rr, secp256k1_ecdsa_ifma_fe8_sub(v, out.p.x)), secp256k1_ecdsa_ifma_fe8_mul_int(secp256k1_ecdsa_ifma_fe8_mul(p.y, j), 2));
    out.z_ratio = secp256k1_ecdsa_ifma_fe8_mul_int(h, 2);
    out.p.z = secp256k1_ecdsa_ifma_fe8_mul(p.z, out.z_ratio);
    return out;
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_jac8 secp256k1_ecdsa_ifma_point_add_affine(secp256k1_ecdsa_ifma_jac8 p, secp256k1_ecdsa_ifma_aff8 q) {
    return secp256k1_ecdsa_ifma_point_add_affine_ratio(p, q).p;
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_fe8 secp256k1_ecdsa_ifma_build_global_table(secp256k1_ecdsa_ifma_aff8 table[SECP256K1_ECDSA_IFMA_Q_TABLE], secp256k1_ecdsa_ifma_fe8 qx, secp256k1_ecdsa_ifma_fe8 qy) {
    secp256k1_ecdsa_ifma_jac8 a;
    secp256k1_ecdsa_ifma_jac8 d;
    secp256k1_ecdsa_ifma_jac8 ai;
    secp256k1_ecdsa_ifma_aff8 d_aff;
    secp256k1_ecdsa_ifma_fe8 c;
    secp256k1_ecdsa_ifma_fe8 c2;
    secp256k1_ecdsa_ifma_fe8 c3;
    secp256k1_ecdsa_ifma_fe8 ratios[SECP256K1_ECDSA_IFMA_Q_TABLE];
    secp256k1_ecdsa_ifma_fe8 zs;
    secp256k1_ecdsa_ifma_add_ratio8 added;
    int i;
    a.x = qx;
    a.y = qy;
    a.z = secp256k1_ecdsa_ifma_fe8_one();
    d = secp256k1_ecdsa_ifma_point_double(a);
    c = d.z;
    c2 = secp256k1_ecdsa_ifma_fe8_sqr(c);
    c3 = secp256k1_ecdsa_ifma_fe8_mul(c2, c);
    table[0].x = secp256k1_ecdsa_ifma_fe8_mul(qx, c2);
    table[0].y = secp256k1_ecdsa_ifma_fe8_mul(qy, c3);
    ratios[0] = c;
    ai.x = table[0].x;
    ai.y = table[0].y;
    ai.z = secp256k1_ecdsa_ifma_fe8_one();
    d_aff.x = d.x;
    d_aff.y = d.y;
    for (i = 1; i < SECP256K1_ECDSA_IFMA_Q_TABLE; i++) {
        added = secp256k1_ecdsa_ifma_point_add_affine_ratio(ai, d_aff);
        ai = added.p;
        ratios[i] = added.z_ratio;
        table[i].x = ai.x;
        table[i].y = ai.y;
    }
    zs = ratios[7];
    for (i = 7; i > 0; i--) {
        secp256k1_ecdsa_ifma_fe8 zs2;
        secp256k1_ecdsa_ifma_fe8 zs3;
        if (i != 7) {
            zs = secp256k1_ecdsa_ifma_fe8_mul(zs, ratios[i]);
        }
        zs2 = secp256k1_ecdsa_ifma_fe8_sqr(zs);
        zs3 = secp256k1_ecdsa_ifma_fe8_mul(zs2, zs);
        table[i - 1].x = secp256k1_ecdsa_ifma_fe8_mul(table[i - 1].x, zs2);
        table[i - 1].y = secp256k1_ecdsa_ifma_fe8_mul(table[i - 1].y, zs3);
    }
    return secp256k1_ecdsa_ifma_fe8_mul(ai.z, c);
}

SECP256K1_ECDSA_IFMA_INLINE void secp256k1_ecdsa_ifma_phi_table(secp256k1_ecdsa_ifma_aff8 out[SECP256K1_ECDSA_IFMA_Q_TABLE], const secp256k1_ecdsa_ifma_aff8 in[SECP256K1_ECDSA_IFMA_Q_TABLE]) {
    secp256k1_ecdsa_ifma_fe8 beta = secp256k1_ecdsa_ifma_fe8_splat_fe(&secp256k1_const_beta);
    int i;
    for (i = 0; i < SECP256K1_ECDSA_IFMA_Q_TABLE; i++) {
        out[i].x = secp256k1_ecdsa_ifma_fe8_mul(in[i].x, beta);
        out[i].y = in[i].y;
    }
}

SECP256K1_ECDSA_IFMA_INLINE __mmask8 secp256k1_ecdsa_ifma_mask8(const uint8_t flags[SECP256K1_ECDSA_IFMA_LANES]) {
    return (__mmask8)(
        (!!flags[0] << 0) | (!!flags[1] << 1) |
        (!!flags[2] << 2) | (!!flags[3] << 3) |
        (!!flags[4] << 4) | (!!flags[5] << 5) |
        (!!flags[6] << 6) | (!!flags[7] << 7)
    );
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_aff8 secp256k1_ecdsa_ifma_lookup_q(const secp256k1_ecdsa_ifma_aff8 table[SECP256K1_ECDSA_IFMA_Q_TABLE], const uint8_t mag[SECP256K1_ECDSA_IFMA_LANES], const uint8_t neg[SECP256K1_ECDSA_IFMA_LANES]) {
    uint64_t offsets[SECP256K1_ECDSA_IFMA_LANES] SECP256K1_ECDSA_IFMA_ALIGN64;
    __m512i index;
    __mmask8 neg_mask = secp256k1_ecdsa_ifma_mask8(neg);
    secp256k1_ecdsa_ifma_aff8 out;
    offsets[0] = (uint64_t)mag[0] * (uint64_t)sizeof(secp256k1_ecdsa_ifma_aff8);
    offsets[1] = (uint64_t)mag[1] * (uint64_t)sizeof(secp256k1_ecdsa_ifma_aff8) + 8;
    offsets[2] = (uint64_t)mag[2] * (uint64_t)sizeof(secp256k1_ecdsa_ifma_aff8) + 16;
    offsets[3] = (uint64_t)mag[3] * (uint64_t)sizeof(secp256k1_ecdsa_ifma_aff8) + 24;
    offsets[4] = (uint64_t)mag[4] * (uint64_t)sizeof(secp256k1_ecdsa_ifma_aff8) + 32;
    offsets[5] = (uint64_t)mag[5] * (uint64_t)sizeof(secp256k1_ecdsa_ifma_aff8) + 40;
    offsets[6] = (uint64_t)mag[6] * (uint64_t)sizeof(secp256k1_ecdsa_ifma_aff8) + 48;
    offsets[7] = (uint64_t)mag[7] * (uint64_t)sizeof(secp256k1_ecdsa_ifma_aff8) + 56;
    index = _mm512_load_si512((const void *)offsets);
#define SECP256K1_ECDSA_IFMA_LOOKUP_Q_LIMB(k) do { \
    out.x.n[(k)] = _mm512_i64gather_epi64(index, (const void *)&table[0].x.n[(k)], 1); \
    out.y.n[(k)] = _mm512_i64gather_epi64(index, (const void *)&table[0].y.n[(k)], 1); \
} while(0)
    SECP256K1_ECDSA_IFMA_LOOKUP_Q_LIMB(0);
    SECP256K1_ECDSA_IFMA_LOOKUP_Q_LIMB(1);
    SECP256K1_ECDSA_IFMA_LOOKUP_Q_LIMB(2);
    SECP256K1_ECDSA_IFMA_LOOKUP_Q_LIMB(3);
    SECP256K1_ECDSA_IFMA_LOOKUP_Q_LIMB(4);
#undef SECP256K1_ECDSA_IFMA_LOOKUP_Q_LIMB
    out.y = secp256k1_ecdsa_ifma_fe8_select(neg_mask, secp256k1_ecdsa_ifma_fe8_neg(out.y), out.y);
    return out;
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_aff8 secp256k1_ecdsa_ifma_lookup_joint(const secp256k1_ecdsa_ifma_prepared8 *p, int window) {
    uint64_t indices[SECP256K1_ECDSA_IFMA_LANES] SECP256K1_ECDSA_IFMA_ALIGN64;
    __m512i index;
    secp256k1_ecdsa_ifma_aff8 out;
    indices[0] = (p->dmag[0][window][0] + (p->dneg[0][window][0] ? 8U : 0U)) * 16U + p->dmag[1][window][0] + (p->dneg[1][window][0] ? 8U : 0U);
    indices[1] = (p->dmag[0][window][1] + (p->dneg[0][window][1] ? 8U : 0U)) * 16U + p->dmag[1][window][1] + (p->dneg[1][window][1] ? 8U : 0U);
    indices[2] = (p->dmag[0][window][2] + (p->dneg[0][window][2] ? 8U : 0U)) * 16U + p->dmag[1][window][2] + (p->dneg[1][window][2] ? 8U : 0U);
    indices[3] = (p->dmag[0][window][3] + (p->dneg[0][window][3] ? 8U : 0U)) * 16U + p->dmag[1][window][3] + (p->dneg[1][window][3] ? 8U : 0U);
    indices[4] = (p->dmag[0][window][4] + (p->dneg[0][window][4] ? 8U : 0U)) * 16U + p->dmag[1][window][4] + (p->dneg[1][window][4] ? 8U : 0U);
    indices[5] = (p->dmag[0][window][5] + (p->dneg[0][window][5] ? 8U : 0U)) * 16U + p->dmag[1][window][5] + (p->dneg[1][window][5] ? 8U : 0U);
    indices[6] = (p->dmag[0][window][6] + (p->dneg[0][window][6] ? 8U : 0U)) * 16U + p->dmag[1][window][6] + (p->dneg[1][window][6] ? 8U : 0U);
    indices[7] = (p->dmag[0][window][7] + (p->dneg[0][window][7] ? 8U : 0U)) * 16U + p->dmag[1][window][7] + (p->dneg[1][window][7] ? 8U : 0U);
    index = _mm512_load_si512((const void *)indices);
#define SECP256K1_ECDSA_IFMA_LOOKUP_JOINT_LIMB(k) do { \
    out.x.n[(k)] = _mm512_i64gather_epi64(index, (const void *)&secp256k1_ecdsa_ifma_joint_x[(k)][0], 8); \
    out.y.n[(k)] = _mm512_i64gather_epi64(index, (const void *)&secp256k1_ecdsa_ifma_joint_y[(k)][0], 8); \
} while(0)
    SECP256K1_ECDSA_IFMA_LOOKUP_JOINT_LIMB(0);
    SECP256K1_ECDSA_IFMA_LOOKUP_JOINT_LIMB(1);
    SECP256K1_ECDSA_IFMA_LOOKUP_JOINT_LIMB(2);
    SECP256K1_ECDSA_IFMA_LOOKUP_JOINT_LIMB(3);
    SECP256K1_ECDSA_IFMA_LOOKUP_JOINT_LIMB(4);
#undef SECP256K1_ECDSA_IFMA_LOOKUP_JOINT_LIMB
    return out;
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_aff8 secp256k1_ecdsa_ifma_map_fixed(const secp256k1_ecdsa_ifma_prepared8 *p, secp256k1_ecdsa_ifma_aff8 q) {
    q.x = secp256k1_ecdsa_ifma_fe8_mul(q.x, p->iso_z2);
    q.y = secp256k1_ecdsa_ifma_fe8_mul(q.y, p->iso_z3);
    return q;
}

static void secp256k1_ecdsa_ifma_words_add_small(uint32_t words[5], uint32_t add) {
    uint64_t carry = add;
    int i;
    for (i = 0; i < 5; i++) {
        uint64_t v = (uint64_t)words[i] + carry;
        words[i] = (uint32_t)v;
        carry = v >> 32;
    }
    VERIFY_CHECK(carry == 0);
}

static void secp256k1_ecdsa_ifma_words_sub_small(uint32_t words[5], uint32_t sub) {
    uint64_t borrow = sub;
    int i;
    for (i = 0; i < 5; i++) {
        uint64_t old = words[i];
        uint64_t low = borrow & UINT64_C(0xFFFFFFFF);
        words[i] = (uint32_t)(old - low);
        borrow = old < low;
    }
    VERIFY_CHECK(borrow == 0);
}

static void secp256k1_ecdsa_ifma_words_shift4(uint32_t words[5]) {
    int i;
    for (i = 0; i < 4; i++) {
        words[i] = (words[i] >> 4) | (words[i + 1] << 28);
    }
    words[4] >>= 4;
}

static void secp256k1_ecdsa_ifma_dense_recode(secp256k1_ecdsa_ifma_prepared8 *p, int base, int lane, const secp256k1_scalar *component) {
    secp256k1_scalar magnitude = *component;
    uint32_t words[5];
    int negative;
    int even;
    int window;
    int i;
    negative = (int)secp256k1_scalar_get_bits_limb32(&magnitude, 255, 1);
    if (negative) {
        secp256k1_scalar_negate(&magnitude, &magnitude);
    }
    for (i = 0; i < 4; i++) {
        words[i] = secp256k1_scalar_get_bits_var(&magnitude, (unsigned int)(32 * i), 32);
    }
    words[4] = 0;
    for (i = 128; i < 256; i += 32) {
        VERIFY_CHECK(secp256k1_scalar_get_bits_var(&magnitude, (unsigned int)i, 32) == 0);
    }
    even = (words[0] & 1U) == 0;
    words[0] |= 1U;
    p->even[base][lane] = (uint8_t)even;
    p->correction_neg[base][lane] = (uint8_t)!negative;
    for (window = 0; window < SECP256K1_ECDSA_IFMA_WINDOWS - 1; window++) {
        int digit = (int)(words[0] & 31U) - 16;
        unsigned int absolute = (unsigned int)(digit < 0 ? -digit : digit);
        p->dmag[base][window][lane] = (uint8_t)((absolute - 1U) >> 1);
        p->dneg[base][window][lane] = (uint8_t)((digit < 0) != negative);
        if (digit < 0) {
            secp256k1_ecdsa_ifma_words_add_small(words, (uint32_t)(-digit));
        } else {
            secp256k1_ecdsa_ifma_words_sub_small(words, (uint32_t)digit);
        }
        secp256k1_ecdsa_ifma_words_shift4(words);
    }
    VERIFY_CHECK(words[1] == 0 && words[2] == 0 && words[3] == 0 && words[4] == 0);
    VERIFY_CHECK(words[0] >= 1 && words[0] <= 15 && (words[0] & 1U));
    p->dmag[base][SECP256K1_ECDSA_IFMA_WINDOWS - 1][lane] = (uint8_t)((words[0] - 1U) >> 1);
    p->dneg[base][SECP256K1_ECDSA_IFMA_WINDOWS - 1][lane] = (uint8_t)negative;
}

static void secp256k1_ecdsa_ifma_prepare_tile(const secp256k1_context *ctx, secp256k1_scalar *rs, secp256k1_scalar *ss, secp256k1_scalar *inverses, const secp256k1_ecdsa_signature *sigs, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) {
        secp256k1_ecdsa_signature_load(ctx, &rs[i], &ss[i], &sigs[i]);
    }
    secp256k1_ecdsa_sig_inverse_var_many(inverses, ss, count);
}

/* Prepare the lanes of p to compute u1[lane]*G + u2[lane]*q[lane]. */
SECP256K1_ECDSA_IFMA_TARGET static void secp256k1_ecdsa_ifma_prepare_mult8(secp256k1_ecdsa_ifma_prepared8 *p, const secp256k1_scalar u1[SECP256K1_ECDSA_IFMA_LANES], const secp256k1_scalar u2[SECP256K1_ECDSA_IFMA_LANES], const secp256k1_ge q[SECP256K1_ECDSA_IFMA_LANES]) {
    secp256k1_fe qx[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_fe qy[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_ecdsa_ifma_fe8 qx8;
    secp256k1_ecdsa_ifma_fe8 qy8;
    int lane;
    for (lane = 0; lane < SECP256K1_ECDSA_IFMA_LANES; lane++) {
        secp256k1_scalar split[4];
        secp256k1_scalar_split_lambda(&split[0], &split[1], &u1[lane]);
        secp256k1_scalar_split_lambda(&split[2], &split[3], &u2[lane]);
        secp256k1_ecdsa_ifma_dense_recode(p, 0, lane, &split[0]);
        secp256k1_ecdsa_ifma_dense_recode(p, 1, lane, &split[1]);
        secp256k1_ecdsa_ifma_dense_recode(p, 2, lane, &split[2]);
        secp256k1_ecdsa_ifma_dense_recode(p, 3, lane, &split[3]);
        qx[lane] = q[lane].x;
        qy[lane] = q[lane].y;
        secp256k1_fe_normalize_var(&qx[lane]);
        secp256k1_fe_normalize_var(&qy[lane]);
    }
    qx8 = secp256k1_ecdsa_ifma_fe8_pack(qx);
    qy8 = secp256k1_ecdsa_ifma_fe8_pack(qy);
    p->global_z = secp256k1_ecdsa_ifma_build_global_table(p->qtab, qx8, qy8);
    p->iso_z2 = secp256k1_ecdsa_ifma_fe8_sqr(p->global_z);
    p->iso_z3 = secp256k1_ecdsa_ifma_fe8_mul(p->iso_z2, p->global_z);
    secp256k1_ecdsa_ifma_phi_table(p->qphi, p->qtab);
}

SECP256K1_ECDSA_IFMA_TARGET static void secp256k1_ecdsa_ifma_prepare8(const secp256k1_context *ctx, secp256k1_ecdsa_ifma_prepared8 *p, const secp256k1_scalar *rs, const secp256k1_scalar *ss, const unsigned char *msghashes32, const secp256k1_pubkey *pubkeys, const secp256k1_scalar *inverses, size_t count) {
    secp256k1_scalar u1[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_scalar u2[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_ge q[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_fe rf[SECP256K1_ECDSA_IFMA_LANES];
    secp256k1_fe rnf[SECP256K1_ECDSA_IFMA_LANES];
    int lane;
    memset(p, 0, sizeof(*p));
    for (lane = 0; lane < SECP256K1_ECDSA_IFMA_LANES; lane++) {
        size_t at = (size_t)lane < count ? (size_t)lane : 0;
        secp256k1_scalar r = rs[at];
        secp256k1_scalar s = ss[at];
        secp256k1_scalar message;
        unsigned char rb32[32];
        int q_ok;
        int range;
        q_ok = secp256k1_pubkey_load(ctx, &q[lane], &pubkeys[at]);
        if (!q_ok) {
            q[lane] = secp256k1_ge_const_g;
        }
        p->eligible[lane] = (uint8_t)((size_t)lane < count && q_ok && !secp256k1_scalar_is_zero(&r) && !secp256k1_scalar_is_zero(&s) && !secp256k1_scalar_is_high(&s));
        secp256k1_scalar_set_b32(&message, &msghashes32[32 * at], NULL);
        secp256k1_scalar_mul(&u1[lane], &message, &inverses[at]);
        secp256k1_scalar_mul(&u2[lane], &r, &inverses[at]);
        secp256k1_scalar_get_b32(rb32, &r);
        range = secp256k1_fe_set_b32_limit(&rf[lane], rb32);
#ifdef VERIFY
        /* We know that rb32 is in range; it comes from a scalar. */
        VERIFY_CHECK(range);
#else
        (void)range;
#endif
        rnf[lane] = rf[lane];
        p->r_plus_n_ok[lane] = (uint8_t)(secp256k1_fe_cmp_var(&rf[lane], &secp256k1_ecdsa_const_p_minus_order) < 0);
        if (p->r_plus_n_ok[lane]) {
            secp256k1_fe_add(&rnf[lane], &secp256k1_ecdsa_const_order_as_fe);
        }
    }
    p->r = secp256k1_ecdsa_ifma_fe8_pack(rf);
    p->r_plus_n = secp256k1_ecdsa_ifma_fe8_pack(rnf);
    secp256k1_ecdsa_ifma_prepare_mult8(p, u1, u2, q);
}

SECP256K1_ECDSA_IFMA_INLINE secp256k1_ecdsa_ifma_aff8 secp256k1_ecdsa_ifma_fixed_correction(const secp256k1_ecdsa_ifma_prepared8 *p, secp256k1_ecdsa_ifma_aff8 q, const uint8_t neg[SECP256K1_ECDSA_IFMA_LANES]) {
    __mmask8 mask = secp256k1_ecdsa_ifma_mask8(neg);
    q = secp256k1_ecdsa_ifma_map_fixed(p, q);
    q.y = secp256k1_ecdsa_ifma_fe8_select(mask, secp256k1_ecdsa_ifma_fe8_neg(q.y), q.y);
    return q;
}

SECP256K1_ECDSA_IFMA_TARGET static void secp256k1_ecdsa_ifma_finish8(uint8_t out[SECP256K1_ECDSA_IFMA_LANES], uint8_t z_zero[SECP256K1_ECDSA_IFMA_LANES], const secp256k1_ecdsa_ifma_prepared8 *p, secp256k1_ecdsa_ifma_jac8 acc) {
    secp256k1_ecdsa_ifma_fe8 final_z = secp256k1_ecdsa_ifma_fe8_mul(acc.z, p->global_z);
    secp256k1_ecdsa_ifma_fe8 z2 = secp256k1_ecdsa_ifma_fe8_sqr(final_z);
    secp256k1_ecdsa_ifma_fe8 d1 = secp256k1_ecdsa_ifma_fe8_sub(acc.x, secp256k1_ecdsa_ifma_fe8_mul(p->r, z2));
    secp256k1_ecdsa_ifma_fe8 d2 = secp256k1_ecdsa_ifma_fe8_sub(acc.x, secp256k1_ecdsa_ifma_fe8_mul(p->r_plus_n, z2));
    int lane;
    for (lane = 0; lane < SECP256K1_ECDSA_IFMA_LANES; lane++) {
        secp256k1_fe z_lane;
        secp256k1_fe d1_lane;
        secp256k1_fe d2_lane;
        int z_is_zero;
        int d1_is_zero;
        int d2_is_zero;
        secp256k1_ecdsa_ifma_fe8_get_lane(&z_lane, &final_z, lane);
        secp256k1_ecdsa_ifma_fe8_get_lane(&d1_lane, &d1, lane);
        secp256k1_ecdsa_ifma_fe8_get_lane(&d2_lane, &d2, lane);
        z_is_zero = secp256k1_fe_normalizes_to_zero_var(&z_lane);
        d1_is_zero = secp256k1_fe_normalizes_to_zero_var(&d1_lane);
        d2_is_zero = secp256k1_fe_normalizes_to_zero_var(&d2_lane);
        out[lane] = (uint8_t)(p->eligible[lane] && !z_is_zero && (d1_is_zero || (p->r_plus_n_ok[lane] && d2_is_zero)));
        z_zero[lane] = (uint8_t)z_is_zero;
    }
}

/* Compute u1*G + u2*Q in every lane prepared by secp256k1_ecdsa_ifma_prepare_mult8,
 * in Jacobian coordinates on the isomorphic curve: the true Z is the result's Z
 * times p->global_z. An exceptional addition sets Z to zero and keeps it zero. */
SECP256K1_ECDSA_IFMA_TARGET static secp256k1_ecdsa_ifma_jac8 secp256k1_ecdsa_ifma_mult8(const secp256k1_ecdsa_ifma_prepared8 *p) {
    secp256k1_ecdsa_ifma_jac8 acc;
    secp256k1_ecdsa_ifma_aff8 q;
    uint8_t zero_mag[SECP256K1_ECDSA_IFMA_LANES];
    int window;
    int base;
    memset(zero_mag, 0, sizeof(zero_mag));
    q = secp256k1_ecdsa_ifma_map_fixed(p, secp256k1_ecdsa_ifma_lookup_joint(p, SECP256K1_ECDSA_IFMA_WINDOWS - 1));
    acc.x = q.x;
    acc.y = q.y;
    acc.z = secp256k1_ecdsa_ifma_fe8_one();
    q = secp256k1_ecdsa_ifma_lookup_q(p->qtab, p->dmag[2][SECP256K1_ECDSA_IFMA_WINDOWS - 1], p->dneg[2][SECP256K1_ECDSA_IFMA_WINDOWS - 1]);
    acc = secp256k1_ecdsa_ifma_point_add_affine(acc, q);
    q = secp256k1_ecdsa_ifma_lookup_q(p->qphi, p->dmag[3][SECP256K1_ECDSA_IFMA_WINDOWS - 1], p->dneg[3][SECP256K1_ECDSA_IFMA_WINDOWS - 1]);
    acc = secp256k1_ecdsa_ifma_point_add_affine(acc, q);
    for (window = SECP256K1_ECDSA_IFMA_WINDOWS - 2; window >= 0; window--) {
        acc = secp256k1_ecdsa_ifma_point_double(acc);
        acc = secp256k1_ecdsa_ifma_point_double(acc);
        acc = secp256k1_ecdsa_ifma_point_double(acc);
        acc = secp256k1_ecdsa_ifma_point_double(acc);
        q = secp256k1_ecdsa_ifma_map_fixed(p, secp256k1_ecdsa_ifma_lookup_joint(p, window));
        acc = secp256k1_ecdsa_ifma_point_add_affine(acc, q);
        q = secp256k1_ecdsa_ifma_lookup_q(p->qtab, p->dmag[2][window], p->dneg[2][window]);
        acc = secp256k1_ecdsa_ifma_point_add_affine(acc, q);
        q = secp256k1_ecdsa_ifma_lookup_q(p->qphi, p->dmag[3][window], p->dneg[3][window]);
        acc = secp256k1_ecdsa_ifma_point_add_affine(acc, q);
    }
    for (base = 0; base < 2; base++) {
        secp256k1_ecdsa_ifma_jac8 corrected;
        __mmask8 mask = secp256k1_ecdsa_ifma_mask8(p->even[base]);
        const uint64_t *x = base == 0 ? secp256k1_ecdsa_ifma_gx : secp256k1_ecdsa_ifma_gphix;
        const uint64_t *y = base == 0 ? secp256k1_ecdsa_ifma_gy : secp256k1_ecdsa_ifma_gphiy;
        q.x = secp256k1_ecdsa_ifma_fe8_splat_limbs(x);
        q.y = secp256k1_ecdsa_ifma_fe8_splat_limbs(y);
        q = secp256k1_ecdsa_ifma_fixed_correction(p, q, p->correction_neg[base]);
        corrected = secp256k1_ecdsa_ifma_point_add_affine(acc, q);
        acc = secp256k1_ecdsa_ifma_jac8_select(mask, corrected, acc);
    }
    for (base = 2; base < 4; base++) {
        secp256k1_ecdsa_ifma_jac8 corrected;
        __mmask8 mask = secp256k1_ecdsa_ifma_mask8(p->even[base]);
        q = secp256k1_ecdsa_ifma_lookup_q(base == 2 ? p->qtab : p->qphi, zero_mag, p->correction_neg[base]);
        corrected = secp256k1_ecdsa_ifma_point_add_affine(acc, q);
        acc = secp256k1_ecdsa_ifma_jac8_select(mask, corrected, acc);
    }
    return acc;
}

/* Convert the lanes of a secp256k1_ecdsa_ifma_mult8 result to affine points with
 * one shared inversion. A lane whose Z is zero, from an exceptional addition or an
 * infinite result, becomes infinity. */
SECP256K1_ECDSA_IFMA_TARGET static void secp256k1_ecdsa_ifma_to_affine8(secp256k1_ge r[SECP256K1_ECDSA_IFMA_LANES], const secp256k1_ecdsa_ifma_prepared8 *p, secp256k1_ecdsa_ifma_jac8 acc) {
    secp256k1_ecdsa_ifma_fe8 final_z = secp256k1_ecdsa_ifma_fe8_mul(acc.z, p->global_z);
    secp256k1_gej j[SECP256K1_ECDSA_IFMA_LANES];
    int lane;
    for (lane = 0; lane < SECP256K1_ECDSA_IFMA_LANES; lane++) {
        secp256k1_ecdsa_ifma_fe8_get_lane(&j[lane].x, &acc.x, lane);
        secp256k1_ecdsa_ifma_fe8_get_lane(&j[lane].y, &acc.y, lane);
        secp256k1_ecdsa_ifma_fe8_get_lane(&j[lane].z, &final_z, lane);
        secp256k1_fe_normalize_var(&j[lane].x);
        secp256k1_fe_normalize_var(&j[lane].y);
        secp256k1_fe_normalize_var(&j[lane].z);
        j[lane].infinity = secp256k1_fe_is_zero(&j[lane].z);
    }
    secp256k1_ge_set_all_gej_var(r, j, SECP256K1_ECDSA_IFMA_LANES);
}

SECP256K1_ECDSA_IFMA_TARGET static void secp256k1_ecdsa_ifma_verify_group8(const secp256k1_context *ctx, uint8_t fast[SECP256K1_ECDSA_IFMA_LANES], uint8_t eligible[SECP256K1_ECDSA_IFMA_LANES], uint8_t z_zero[SECP256K1_ECDSA_IFMA_LANES], const secp256k1_scalar *rs, const secp256k1_scalar *ss, const unsigned char *msghashes32, const secp256k1_pubkey *pubkeys, const secp256k1_scalar *inverses, size_t count) {
    secp256k1_ecdsa_ifma_prepared8 p;
    int lane;
    secp256k1_ecdsa_ifma_prepare8(ctx, &p, rs, ss, msghashes32, pubkeys, inverses, count);
    secp256k1_ecdsa_ifma_finish8(fast, z_zero, &p, secp256k1_ecdsa_ifma_mult8(&p));
    for (lane = 0; lane < SECP256K1_ECDSA_IFMA_LANES; lane++) {
        eligible[lane] = p.eligible[lane];
    }
}

/* Verify every input independently. An exceptional addition in the incomplete
 * mixed-add path can only set Z to zero and thus cause rejection; subsequent
 * formulas keep Z zero. With a nonzero Z every addition was regular, so the
 * comparison with r is exact and a rejection is final. Only eligible lanes
 * whose Z is zero, from an exceptional addition or an infinite result, are
 * rechecked with the existing ECDSA verifier. */
SECP256K1_ECDSA_IFMA_TARGET static void secp256k1_ecdsa_ifma_verify_many(const secp256k1_context *ctx, uint8_t *out, const secp256k1_ecdsa_signature *sigs, const unsigned char *msghashes32, const secp256k1_pubkey *pubkeys, size_t count) {
    secp256k1_scalar rs[SECP256K1_ECDSA_IFMA_TILE];
    secp256k1_scalar ss[SECP256K1_ECDSA_IFMA_TILE];
    secp256k1_scalar inverses[SECP256K1_ECDSA_IFMA_TILE];
    size_t tile_base;
    for (tile_base = 0; tile_base < count; tile_base += SECP256K1_ECDSA_IFMA_TILE) {
        size_t tile_count = count - tile_base;
        size_t group_base;
        if (tile_count > SECP256K1_ECDSA_IFMA_TILE) {
            tile_count = SECP256K1_ECDSA_IFMA_TILE;
        }
        secp256k1_ecdsa_ifma_prepare_tile(ctx, rs, ss, inverses, sigs + tile_base, tile_count);
        for (group_base = 0; group_base < tile_count; group_base += SECP256K1_ECDSA_IFMA_LANES) {
            uint8_t fast[SECP256K1_ECDSA_IFMA_LANES];
            uint8_t eligible[SECP256K1_ECDSA_IFMA_LANES];
            uint8_t z_zero[SECP256K1_ECDSA_IFMA_LANES];
            size_t group_count = tile_count - group_base;
            size_t lane;
            if (group_count > SECP256K1_ECDSA_IFMA_LANES) {
                group_count = SECP256K1_ECDSA_IFMA_LANES;
            }
            secp256k1_ecdsa_ifma_verify_group8(ctx, fast, eligible, z_zero, rs + group_base, ss + group_base, msghashes32 + 32 * (tile_base + group_base), pubkeys + tile_base + group_base, inverses + group_base, group_count);
            for (lane = 0; lane < group_count; lane++) {
                size_t index = tile_base + group_base + lane;
                if (fast[lane]) {
                    out[index] = 1;
                } else if (!eligible[lane] || !z_zero[lane]) {
                    out[index] = 0;
                } else {
                    out[index] = (uint8_t)secp256k1_ecdsa_verify(ctx, &sigs[index], &msghashes32[32 * index], &pubkeys[index]);
                }
            }
        }
    }
}


#endif /* SECP256K1_ECDSA_IFMA_IMPL_H */
