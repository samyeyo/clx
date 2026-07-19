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
    inline int clx_ctz(unsigned x) { unsigned long i; _BitScanForward(&i, x); return static_cast<int>(i); }
    #ifdef _WIN64
        inline int clx_ctzll(unsigned long long x) { unsigned long i; _BitScanForward64(&i, x); return static_cast<int>(i); }
    #else
        inline int clx_ctzll(unsigned long long x) {
            if (static_cast<unsigned>(x)) return clx_ctz(static_cast<unsigned>(x));
            return clx_ctz(static_cast<unsigned>(x >> 32)) + 32;
        }
    #endif
#else
    inline int clx_ctz(unsigned x) { return __builtin_ctz(x); }
    inline int clx_ctzll(unsigned long long x) { return __builtin_ctzll(x); }
#endif

//------------------ clx_find_first_nil : scans types[0..size) and returns the index of the first nil, returns size if none found.
inline size_t clx_find_first_nil(const uint8_t* types, size_t size) {
    size_t i = 0;

    //------------------ 32-byte AVX2 path
#if defined(CLX_HAS_AVX2)
    const __m256i zero256 = _mm256_setzero_si256();
    for (; i + 32 <= size; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(types + i));
        __m256i cmp = _mm256_cmpeq_epi8(v, zero256);
        uint32_t mask = ~_mm256_movemask_epi8(cmp);
        if (mask != 0) {
            return i + clx_ctz(mask);
        }
    }
    //------------------ 16-byte SSE2 path
#elif defined(CLX_HAS_SSE2)
    const __m128i zero = _mm_setzero_si128();
    for (; i + 16 <= size; i += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(types + i));
        __m128i cmp = _mm_cmpeq_epi8(v, zero);
        int mask = ~_mm_movemask_epi8(cmp);
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
        uint8x8_t narrow = vmovn_u16(vreinterpretq_u16_u8(cmp));
        uint64_t low64 = vget_lane_u64(vreinterpret_u64_u8(narrow), 0);
        if (low64 != 0) {
            for (size_t j = 0; j < 16 && i + j < size; ++j) {
                if (types[i + j] == 0) return i + j;
            }
        }
    }
#endif

    //------------------ Scalar tail
    for (; i < size; ++i) {
        if (types[i] == 0) return i;
    }
    return size;
}

//------------------ clx_find_first_nonnil : scans types[start..size) and returns the index of the first non-nil, returns size if none found.
inline size_t clx_find_first_nonnil(const uint8_t* types, size_t size, size_t start = 0) {
    size_t i = start;

    //------------------ 32-byte AVX2 path
#if defined(CLX_HAS_AVX2)
    const __m256i zero256 = _mm256_setzero_si256();
    for (; i + 32 <= size; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(types + i));
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
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(types + i));
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
        uint8x8_t narrow = vmovn_u16(vreinterpretq_u16_u8(cmp));
        uint64_t low64 = vget_lane_u64(vreinterpret_u64_u8(narrow), 0);
        if (low64 != 0xFFFFFFFFFFFFFFFFULL) {
            for (size_t j = 0; j < 16 && i + j < size; ++j) {
                if (types[i + j] != 0) return i + j;
            }
        }
    }
#endif

    //------------------ Scalar tail
    for (; i < size; ++i) {
        if (types[i] != 0) return i;
    }
    return size;
}

//------------------ clx_type_mask_nonnil : returns a bitmask where bit k is 1 if types[offset + k] != 0, used for GC batch scanning.
//  32-byte AVX2 path returns 32-bit mask; 16-byte SSE2/NEON path returns 16-bit mask.
inline uint32_t clx_type_mask_nonnil(const uint8_t* types, size_t offset, size_t count) {
    (void)count;
    //------------------ 32-byte AVX2 path
#if defined(CLX_HAS_AVX2)
    const __m256i zero256 = _mm256_setzero_si256();
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(types + offset));
    __m256i cmp = _mm256_cmpeq_epi8(v, zero256);
    return ~_mm256_movemask_epi8(cmp);
    //------------------ 16-byte SSE2 path
#elif defined(CLX_HAS_SSE2)
    const __m128i zero = _mm_setzero_si128();
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(types + offset));
    __m128i cmp = _mm_cmpeq_epi8(v, zero);
    return static_cast<uint32_t>(~_mm_movemask_epi8(cmp)) & 0xFFFF;
    //------------------ 16-byte NEON path
#elif defined(CLX_HAS_NEON)
    const uint8x16_t zero = vdupq_n_u8(0);
    uint8x16_t v = vld1q_u8(types + offset);
    uint8x16_t cmp = vceqq_u8(v, zero);
    uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(cmp), 4);
    uint64_t bits = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
    return static_cast<uint32_t>(~bits) & 0xFFFF;
    //------------------ Scalar fallback
#else
    uint32_t mask = 0;
    for (size_t k = 0; k < 16; ++k) {
        if (types[offset + k] != 0) mask |= (1u << k);
    }
    return mask;
#endif
}

//------------------ clx_type_mask32_nonnil : returns a 32-bit bitmask for 32 bytes at offset.
//  Used by GC mark loop for AVX2-accelerated scanning.
#if defined(CLX_HAS_AVX2)
inline uint32_t clx_type_mask32_nonnil(const uint8_t* types, size_t offset) {
    const __m256i zero256 = _mm256_setzero_si256();
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(types + offset));
    __m256i cmp = _mm256_cmpeq_epi8(v, zero256);
    return ~_mm256_movemask_epi8(cmp);
}
#endif

//------------------ clx_validate_types_range : validates that all types in [start..start+count) are in [lo, hi] using SIMD.
//  Returns true if all valid, false if any invalid. For table_concat validation.
inline bool clx_validate_types_range(const uint8_t* types, size_t start, size_t count, uint8_t lo, uint8_t hi) {
    size_t k = start;
    size_t end = start + count;

    //------------------ 32-byte AVX2 path
#if defined(CLX_HAS_AVX2)
    const __m256i lo256 = _mm256_set1_epi8(static_cast<char>(lo));
    const __m256i hi256 = _mm256_set1_epi8(static_cast<char>(hi));
    for (; k + 32 <= end; k += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(types + k));
        __m256i ge_lo = _mm256_or_si256(_mm256_cmpgt_epi8(v, lo256), _mm256_cmpeq_epi8(v, lo256));
        __m256i le_hi = _mm256_or_si256(_mm256_cmpgt_epi8(hi256, v), _mm256_cmpeq_epi8(hi256, v));
        __m256i ok = _mm256_and_si256(ge_lo, le_hi);
        if (_mm256_movemask_epi8(ok) != -1) return false;
    }
    //------------------ 16-byte SSE2 path
#elif defined(CLX_HAS_SSE2)
    const __m128i lo128 = _mm_set1_epi8(static_cast<char>(lo));
    const __m128i hi128 = _mm_set1_epi8(static_cast<char>(hi));
    for (; k + 16 <= end; k += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(types + k));
        __m128i ge_lo = _mm_or_si128(_mm_cmpgt_epi8(v, lo128), _mm_cmpeq_epi8(v, lo128));
        __m128i le_hi = _mm_or_si128(_mm_cmpgt_epi8(hi128, v), _mm_cmpeq_epi8(hi128, v));
        __m128i ok = _mm_and_si128(ge_lo, le_hi);
        if (_mm_movemask_epi8(ok) != 0xFFFF) return false;
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
            if (lane_vals[bit]) return false;
        }
    }
#endif

    //------------------ Scalar tail
    for (; k < end; ++k) {
        if (types[k] < lo || types[k] > hi) return false;
    }
    return true;
}

//------------------ clx_rawlen_array : SIMD-optimized rawlen for the array part of a table, returns the count of leading non-nil elements.
inline size_t clx_rawlen_array(const uint8_t* types, size_t array_size) {
    return clx_find_first_nil(types, array_size);
}

#endif // CLX_SIMD_H
