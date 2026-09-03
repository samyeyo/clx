// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  clx_simd.h · Cross-platform SIMD helpers   │
// └─────────────────────────────────────────────┘

#ifndef CLX_SIMD_H
#define CLX_SIMD_H

#include <cstddef>
#include <cstdint>

#ifndef CLX_INLINE_HOT
#define CLX_INLINE_HOT inline
#endif
#ifndef CLX_INLINE
#define CLX_INLINE inline
#endif

//------------------ Platform detection
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define CLX_HAS_SSE2 1
#include <emmintrin.h>
#endif

#if defined(__AVX2__)
#define CLX_HAS_AVX2 1
#include <immintrin.h>
#endif

#if defined(__SSSE3__) || defined(__SSE3__)
#define CLX_HAS_SSSE3 1
#include <tmmintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define CLX_HAS_NEON 1
#include <arm_neon.h>
#endif

//------------------ Portable count-trailing-zeros
#ifdef _MSC_VER
#include <intrin.h>

CLX_INLINE_HOT int clx_ctz(unsigned x) {
    unsigned long i;
    _BitScanForward(&i, x);
    return static_cast<int>(i);
}
#ifdef _WIN64
CLX_INLINE_HOT int clx_ctzll(unsigned long long x) {
    unsigned long i;
    _BitScanForward64(&i, x);
    return static_cast<int>(i);
}
#else
CLX_INLINE_HOT int clx_ctzll(unsigned long long x) {
    if (static_cast<unsigned>(x))
        return clx_ctz(static_cast<unsigned>(x));
    return clx_ctz(static_cast<unsigned>(x >> 32)) + 32;
}
#endif
#else
CLX_INLINE_HOT int clx_ctz(unsigned x) {
    return __builtin_ctz(x);
}

CLX_INLINE_HOT int clx_ctzll(unsigned long long x) {
    return __builtin_ctzll(x);
}
#endif

//------------------ clx_find_first_nil : scans types[0..size) and returns the index of the first nil, returns size if none found.
CLX_INLINE_HOT size_t clx_find_first_nil(const uint8_t *types, size_t size) {
    size_t i = 0;

    //------------------ 32-byte AVX2 path
#if defined(CLX_HAS_AVX2)
    const __m256i zero256 = _mm256_setzero_si256();
    for (; i + 32 <= size; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(types + i));
        __m256i cmp = _mm256_cmpeq_epi8(v, zero256);
        uint32_t mask = _mm256_movemask_epi8(cmp);
        if (mask != 0) {
            return i + clx_ctz(mask);
        }
    }
    //------------------ 16-byte SSE2 path
#elif defined(CLX_HAS_SSE2)
    const __m128i zero = _mm_setzero_si128();
    for (; i + 16 <= size; i += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(types + i));
        __m128i cmp = _mm_cmpeq_epi8(v, zero);
        int mask = _mm_movemask_epi8(cmp);
        if (mask != 0) {
            return i + clx_ctz(static_cast<unsigned>(mask));
        }
    }
    //------------------ 16-byte NEON path
#elif defined(CLX_HAS_NEON)
    const uint8x16_t zero = vdupq_n_u8(0);
    for (; i + 16 <= size; i += 16) {
        uint8x16_t v = vld1q_u8(types + i);
        uint8x16_t cmp = vceqq_u8(v, zero);
        uint64x2_t lanes = vreinterpretq_u64_u8(cmp);
        uint64_t low64_0 = vgetq_lane_u64(lanes, 0);
        uint64_t low64_1 = vgetq_lane_u64(lanes, 1);
        if (low64_0 != 0 || low64_1 != 0) {
            for (size_t j = 0; j < 16 && i + j < size; ++j) {
                if (types[i + j] == 0)
                    return i + j;
            }
        }
    }
#endif

    //------------------ Scalar tail
    for (; i < size; ++i) {
        if (types[i] == 0)
            return i;
    }
    return size;
}

//------------------ clx_find_first_nonnil : scans types[start..size) and returns the index of the first non-nil, returns size if none found.
CLX_INLINE_HOT size_t clx_find_first_nonnil(const uint8_t *types, size_t size, size_t start = 0) {
    size_t i = start;

    //------------------ 32-byte AVX2 path
#if defined(CLX_HAS_AVX2)
    const __m256i zero256 = _mm256_setzero_si256();
    for (; i + 32 <= size; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(types + i));
        __m256i cmp = _mm256_cmpeq_epi8(v, zero256);
        uint32_t mask = _mm256_movemask_epi8(cmp);
        if (mask != 0xFFFFFFFF) {
            uint32_t nz = ~mask;
            return i + clx_ctz(nz);
        }
    }
    //------------------ 16-byte SSE2 path
#elif defined(CLX_HAS_SSE2)
    const __m128i zero = _mm_setzero_si128();
    for (; i + 16 <= size; i += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(types + i));
        __m128i cmp = _mm_cmpeq_epi8(v, zero);
        int mask = _mm_movemask_epi8(cmp);
        if (mask != 0xFFFF) {
            int nz = ~mask & 0xFFFF;
            return i + clx_ctz(static_cast<unsigned>(nz));
        }
    }
    //------------------ 16-byte NEON path
#elif defined(CLX_HAS_NEON)
    const uint8x16_t zero = vdupq_n_u8(0);
    for (; i + 16 <= size; i += 16) {
        uint8x16_t v = vld1q_u8(types + i);
        uint8x16_t cmp = vceqq_u8(v, zero);
        uint64x2_t lanes = vreinterpretq_u64_u8(cmp);
        uint64_t low64_0 = vgetq_lane_u64(lanes, 0);
        uint64_t low64_1 = vgetq_lane_u64(lanes, 1);
        if (low64_0 != 0xFFFFFFFFFFFFFFFFULL || low64_1 != 0xFFFFFFFFFFFFFFFFULL) {
            for (size_t j = 0; j < 16 && i + j < size; ++j) {
                if (types[i + j] != 0)
                    return i + j;
            }
        }
    }
#endif

    //------------------ Scalar tail
    for (; i < size; ++i) {
        if (types[i] != 0)
            return i;
    }
    return size;
}

//------------------ clx_validate_types_range : validates that all types in [start..start+count) are in [lo, hi] using SIMD.
CLX_INLINE_HOT bool clx_validate_types_range(const uint8_t *types, size_t start, size_t count, uint8_t lo, uint8_t hi) {
    size_t k = start;
    size_t end = start + count;

    //------------------ 32-byte AVX2 path
#if defined(CLX_HAS_AVX2)
    const __m256i lo256 = _mm256_set1_epi8(static_cast<char>(lo));
    const __m256i hi256 = _mm256_set1_epi8(static_cast<char>(hi));
    for (; k + 32 <= end; k += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(types + k));
        __m256i ge_lo = _mm256_or_si256(_mm256_cmpgt_epi8(v, lo256), _mm256_cmpeq_epi8(v, lo256));
        __m256i le_hi = _mm256_or_si256(_mm256_cmpgt_epi8(hi256, v), _mm256_cmpeq_epi8(hi256, v));
        __m256i ok = _mm256_and_si256(ge_lo, le_hi);
        if (_mm256_movemask_epi8(ok) != -1)
            return false;
    }
    //------------------ 16-byte SSE2 path
#elif defined(CLX_HAS_SSE2)
    const __m128i lo128 = _mm_set1_epi8(static_cast<char>(lo));
    const __m128i hi128 = _mm_set1_epi8(static_cast<char>(hi));
    for (; k + 16 <= end; k += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(types + k));
        __m128i ge_lo = _mm_or_si128(_mm_cmpgt_epi8(v, lo128), _mm_cmpeq_epi8(v, lo128));
        __m128i le_hi = _mm_or_si128(_mm_cmpgt_epi8(hi128, v), _mm_cmpeq_epi8(hi128, v));
        __m128i ok = _mm_and_si128(ge_lo, le_hi);
        if (_mm_movemask_epi8(ok) != 0xFFFF)
            return false;
    }
    //------------------ 16-byte NEON path
#elif defined(CLX_HAS_NEON)
    const uint8x16_t lo16 = vdupq_n_u8(lo);
    const uint8x16_t hi16 = vdupq_n_u8(hi);
    for (; k + 16 <= end; k += 16) {
        uint8x16_t v = vld1q_u8(types + k);
        uint8x16_t ok = vandq_u8(vcgeq_u8(v, lo16), vcleq_u8(v, hi16));
        uint8_t lane_vals[16];
        vst1q_u8(lane_vals, vmvnq_u8(ok));
        for (int bit = 0; bit < 16; ++bit) {
            if (lane_vals[bit])
                return false;
        }
    }
#endif

    //------------------ Scalar tail
    for (; k < end; ++k) {
        if (types[k] < lo || types[k] > hi)
            return false;
    }
    return true;
}

//======================================================================
// String helpers (SIMD) — used by src/runtime/strings.cpp
//======================================================================

//------------------ clx_case_fold : ASCII case conversion (dst may equal src).
CLX_INLINE_HOT void clx_case_fold(char *dst, const char *src, size_t n, int to_lower) {
    size_t i = 0;

    //------------------ 32-byte AVX2 path
#if defined(CLX_HAS_AVX2)
    {
        const __m256i lo_bound = _mm256_set1_epi8(static_cast<char>(to_lower ? 'A' : 'a'));
        const __m256i hi_bound = _mm256_set1_epi8(static_cast<char>(to_lower ? 'Z' : 'z'));
        const __m256i flip = _mm256_set1_epi8(static_cast<char>(0x20));
        const __m256i one = _mm256_set1_epi8(1);
        for (; i + 32 <= n; i += 32) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + i));
            // in_range = (lo <= v) & (v <= hi) using unsigned-style compare via lo-1 < v
            __m256i ge
                = _mm256_or_si256(_mm256_cmpgt_epi8(v, _mm256_sub_epi8(lo_bound, one)), _mm256_cmpeq_epi8(v, lo_bound));
            __m256i le = _mm256_or_si256(_mm256_cmpgt_epi8(hi_bound, v), _mm256_cmpeq_epi8(hi_bound, v));
            __m256i in_range = _mm256_and_si256(ge, le);
            __m256i out = _mm256_xor_si256(v, _mm256_and_si256(in_range, flip));
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + i), out);
        }
    }
    //------------------ 16-byte SSE2 path
#elif defined(CLX_HAS_SSE2)
    {
        const __m128i lo_bound = _mm_set1_epi8(static_cast<char>(to_lower ? 'A' : 'a'));
        const __m128i hi_bound = _mm_set1_epi8(static_cast<char>(to_lower ? 'Z' : 'z'));
        const __m128i flip = _mm_set1_epi8(static_cast<char>(0x20));
        const __m128i one = _mm_set1_epi8(1);
        for (; i + 16 <= n; i += 16) {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
            __m128i ge = _mm_or_si128(_mm_cmpgt_epi8(v, _mm_sub_epi8(lo_bound, one)), _mm_cmpeq_epi8(v, lo_bound));
            __m128i le = _mm_or_si128(_mm_cmpgt_epi8(hi_bound, v), _mm_cmpeq_epi8(hi_bound, v));
            __m128i in_range = _mm_and_si128(ge, le);
            __m128i out = _mm_xor_si128(v, _mm_and_si128(in_range, flip));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), out);
        }
    }
    //------------------ 16-byte NEON path
#elif defined(CLX_HAS_NEON)
    {
        const uint8x16_t lo_bound = vdupq_n_u8(static_cast<uint8_t>(to_lower ? 'A' : 'a'));
        const uint8x16_t hi_bound = vdupq_n_u8(static_cast<uint8_t>(to_lower ? 'Z' : 'z'));
        const uint8x16_t flip = vdupq_n_u8(0x20);
        for (; i + 16 <= n; i += 16) {
            uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t *>(src + i));
            uint8x16_t in_range = vandq_u8(vcgeq_u8(v, lo_bound), vcleq_u8(v, hi_bound));
            uint8x16_t out = veorq_u8(v, vandq_u8(in_range, flip));
            vst1q_u8(reinterpret_cast<uint8_t *>(dst + i), out);
        }
    }
#else
    (void)dst;
    (void)src;
    (void)to_lower;
#endif

    //------------------ Scalar tail
    if (to_lower) {
        for (; i < n; ++i)
            dst[i] = static_cast<char>((src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i]);
    } else {
        for (; i < n; ++i)
            dst[i] = static_cast<char>((src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i]);
    }
}

//------------------ clx_reverse_bytes : reverse n bytes (dst must not overlap src).
CLX_INLINE_HOT void clx_reverse_bytes(char *dst, const char *src, size_t n) {
    size_t i = 0;
#if defined(CLX_HAS_SSSE3)
    const __m128i rev = _mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    for (; i + 16 <= n; i += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + n - i - 16));
        __m128i out = _mm_shuffle_epi8(v, rev);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), out);
    }
    //------------------ scalar middle + head
    for (size_t j = n - i; j > i; --j, ++i) {
        dst[i] = src[j - 1];
    }
#elif defined(CLX_HAS_NEON)
    static const uint8_t rev_idx[16] = { 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
    const uint8x16_t rev = vld1q_u8(rev_idx);
    for (; i + 16 <= n; i += 16) {
        uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t *>(src + n - i - 16));
        uint8x16_t out = vqtbl1q_u8(v, rev);
        vst1q_u8(reinterpret_cast<uint8_t *>(dst + i), out);
    }
    for (size_t j = n - i; j > i; --j, ++i) {
        dst[i] = src[j - 1];
    }
#else
    for (; i < n; ++i)
        dst[i] = src[n - 1 - i];
#endif
}

//------------------ clx_find_byte_of_set : returns 1 if s[0..n) contains any byte of "^$*+?.([%-", else 0.
CLX_INLINE_HOT int clx_find_byte_of_set(const char *s, size_t n) {
    size_t i = 0;
#if defined(CLX_HAS_AVX2) || defined(CLX_HAS_SSE2) || defined(CLX_HAS_NEON)
    static const char kSpecials[8] = { '^', '$', '*', '+', '?', '.', '(', '[' };
    static const char kSpecials2[8] = { '%', '-', 0, 0, 0, 0, 0, 0 };
#endif

    //------------------ 32-byte AVX2 path
#if defined(CLX_HAS_AVX2)
    {
        const __m256i sp1v = _mm256_set1_epi64x(*reinterpret_cast<const long long *>(kSpecials));
        const __m256i sp2v = _mm256_set1_epi64x(*reinterpret_cast<const long long *>(kSpecials2));
        for (; i + 32 <= n; i += 32) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(s + i));
            __m256i c1 = _mm256_cmpeq_epi8(v, sp1v);
            __m256i c2 = _mm256_cmpeq_epi8(v, sp2v);
            uint32_t mask = _mm256_movemask_epi8(_mm256_or_si256(c1, c2));
            if (mask)
                return 1;
        }
    }
    //------------------ 16-byte SSE2 path
#elif defined(CLX_HAS_SSE2)
    {
        const __m128i sp1v = _mm_set1_epi64x(*reinterpret_cast<const long long *>(kSpecials));
        const __m128i sp2v = _mm_set1_epi64x(*reinterpret_cast<const long long *>(kSpecials2));
        for (; i + 16 <= n; i += 16) {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s + i));
            __m128i c1 = _mm_cmpeq_epi8(v, sp1v);
            __m128i c2 = _mm_cmpeq_epi8(v, sp2v);
            int mask = _mm_movemask_epi8(_mm_or_si128(c1, c2));
            if (mask)
                return 1;
        }
    }
    //------------------ 16-byte NEON path
#elif defined(CLX_HAS_NEON)
    {
        uint8x16_t sp1v = vld1q_u8(reinterpret_cast<const uint8_t *>(kSpecials));
        uint8x16_t sp2v = vld1q_u8(reinterpret_cast<const uint8_t *>(kSpecials2));
        // broadcast the 8-byte sets to both halves
        sp1v = vcombine_u8(vget_low_u8(sp1v), vget_low_u8(sp1v));
        sp2v = vcombine_u8(vget_low_u8(sp2v), vget_low_u8(sp2v));
        for (; i + 16 <= n; i += 16) {
            uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t *>(s + i));
            uint8x16_t c1 = vceqq_u8(v, sp1v);
            uint8x16_t c2 = vceqq_u8(v, sp2v);
            uint64x2_t lanes = vreinterpretq_u64_u8(vorrq_u8(c1, c2));
            if (vgetq_lane_u64(lanes, 0) != 0 || vgetq_lane_u64(lanes, 1) != 0)
                return 1;
        }
    }
#endif

    for (; i < n; ++i) {
        char c = s[i];
        if (c == '^' || c == '$' || c == '*' || c == '+' || c == '?' || c == '.' || c == '(' || c == '[' || c == '%'
            || c == '-')
            return 1;
    }
    return 0;
}

#endif
