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
#else
    inline int clx_ctz(unsigned x) { return __builtin_ctz(x); }
#endif

//------------------ clx_find_first_nil : scans types[0..size) and returns the index of the first, returns size if none found.
inline size_t clx_find_first_nil(const uint8_t* types, size_t size) {
    size_t i = 0;

    //------------------ 16-byte SSE2 path
#if defined(CLX_HAS_SSE2)
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

//------------------ clx_find_first_nonnil : scans types[start..size) and returns the index of the first, returns size if none found.
inline size_t clx_find_first_nonnil(const uint8_t* types, size_t size, size_t start = 0) {
    size_t i = start;

    //------------------ 16-byte SSE2 path
#if defined(CLX_HAS_SSE2)
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

//------------------ clx_type_mask_nonnil : returns a 16-bit bitmask where bit k is 1 if types[offset + k] != 0, used for GC batch scanning.
inline uint32_t clx_type_mask_nonnil(const uint8_t* types, size_t offset, size_t count) {
    (void)count;
    //------------------ 16-byte SSE2 path
#if defined(CLX_HAS_SSE2)
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

//------------------ clx_rawlen_array : SIMD-optimized rawlen for the array part of a table, returns the count of leading non-nil elements.
inline size_t clx_rawlen_array(const uint8_t* types, size_t array_size) {
    return clx_find_first_nil(types, array_size);
}

#endif // CLX_SIMD_H
