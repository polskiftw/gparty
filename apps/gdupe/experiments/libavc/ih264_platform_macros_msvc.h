/*
 * Windows/MSVC x64 adaptation of AOSP libavc's x86 platform macro layer.
 *
 * This file replaces compiler/ABI glue only for the gdupe static decoder
 * build. H.264 codec logic remains at the pinned upstream commit.
 */

#ifndef _IH264_PLATFORM_MACROS_H_
#define _IH264_PLATFORM_MACROS_H_

#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <intrin.h>
#include <immintrin.h>

/* MSVC has no GNU may_alias/packed expression used by upstream loadu_32.
 * memcpy is alignment-safe and MSVC optimizes this fixed four-byte copy. */
static __forceinline __m128i loadu_32(const void *p)
{
    int32_t value;
    memcpy(&value, p, sizeof(value));
    return _mm_cvtsi32_si128(value);
}

#define CLIP_U8(x)  CLIP3(0, UINT8_MAX, (x))
#define CLIP_S8(x)  CLIP3(INT8_MIN, INT8_MAX, (x))
#define CLIP_U10(x) CLIP3(0, 1023, (x))
#define CLIP_S10(x) CLIP3(-512, 511, (x))
#define CLIP_U11(x) CLIP3(0, 2047, (x))
#define CLIP_S11(x) CLIP3(-1024, 1023, (x))
#define CLIP_U12(x) CLIP3(0, 4095, (x))
#define CLIP_S12(x) CLIP3(-2048, 2047, (x))
#define CLIP_U16(x) CLIP3(0, UINT16_MAX, (x))
#define CLIP_S16(x) CLIP3(INT16_MIN, INT16_MAX, (x))
#define CLIP_U32(x) CLIP3(0, UINT32_MAX, (x))
#define CLIP_S32(x) CLIP3(INT32_MIN, INT32_MAX, (x))

#define SHL(x, y) (((y) < 32) ? ((x) << (y)) : 0)
#define SHR(x, y) (((y) < 32) ? ((x) >> (y)) : 0)
#define SHR_NEG(val, shift) ((shift > 0) ? ((val) >> (shift)) : ((val) << (-(shift))))
#define SHL_NEG(val, shift) ((shift < 0) ? ((val) >> (-(shift))) : ((val) << (shift)))

#define ITT_BIG_ENDIAN(x) ((UWORD32)_byteswap_ulong((unsigned long)(x)))

#define NOP(nop_cnt)                                                   \
    do                                                                 \
    {                                                                  \
        UWORD32 nop_i;                                                 \
        for(nop_i = 0; nop_i < (UWORD32)(nop_cnt); ++nop_i) __nop(); \
    } while(0)

#define PLD(a)

static __forceinline UWORD32 CLZ(UWORD32 word)
{
    unsigned long index;
    if(word == 0)
        return 31;
    _BitScanReverse(&index, (unsigned long)word);
    return (UWORD32)(31u - index);
}

static __forceinline UWORD32 CTZ(UWORD32 word)
{
    unsigned long index;
    if(word == 0)
        return 31;
    _BitScanForward(&index, (unsigned long)word);
    return (UWORD32)index;
}

#define DATA_SYNC() _mm_mfence()
/* A few decoder translation units call GCC's barrier builtin directly. */
#define __sync_synchronize() _mm_mfence()

#define INLINE __forceinline

#define PREFETCH_ENABLE 1
#if PREFETCH_ENABLE
#define PREFETCH(ptr, type) _mm_prefetch((const char *)(ptr), (type))
#else
#define PREFETCH(ptr, type)
#endif

#define MEM_ALIGN8  __declspec(align(8))
#define MEM_ALIGN16 __declspec(align(16))
#define MEM_ALIGN32 __declspec(align(32))

#endif /* _IH264_PLATFORM_MACROS_H_ */
