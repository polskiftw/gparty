/******************************************************************************
*
* Copyright (C) 2012 Ittiam Systems Pvt Ltd, Bangalore
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at:
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
******************************************************************************/
/*
 * Modified for gdupe on 2026-08-15.
 * Windows/MSVC x64 adaptation of libhevc's x86 platform macro layer.
 * Kept intentionally narrow for the gdupe decoder-only build.
 */

#ifndef _IHEVC_PLATFORM_MACROS_H_
#define _IHEVC_PLATFORM_MACROS_H_

#include <intrin.h>
#include <immintrin.h>

#define CLIP_U8(x) CLIP3((x), 0, 255);
#define CLIP_S8(x) CLIP3((x), -128, 127);
#define CLIP_U10(x) CLIP3((x), 0, 1023);
#define CLIP_S10(x) CLIP3((x), -512, 511);
#define CLIP_U12(x) CLIP3((x), 0, 4095);
#define CLIP_S12(x) CLIP3((x), -2048, 2047);
#define CLIP_U14(x) CLIP3((x), 0, 16383);
#define CLIP_S14(x) CLIP3((x), -8192, 8191);
#define CLIP_U16(x) CLIP3((x), 0, 65535);
#define CLIP_S16(x) CLIP3((x), -32768, 32767);

#define SHL(x, y) (((y) < 32) ? ((x) << (y)) : 0)
#define SHR(x, y) (((y) < 32) ? ((x) >> (y)) : 0)
#define SHR_NEG(val, shift) ((shift > 0) ? (val >> shift) : (val << (-shift)))
#define SHL_NEG(val, shift) ((shift < 0) ? (val >> (-shift)) : (val << shift))

#define ITT_BIG_ENDIAN(x)       ((x << 24))             | \
                                ((x & 0x0000ff00) << 8) | \
                                ((x & 0x00ff0000) >> 8) | \
                                ((UWORD32)x >> 24);

#define NOP(nop_cnt)                                                    \
    {                                                                   \
        UWORD32 nop_i;                                                  \
        for(nop_i = 0; nop_i < (UWORD32)(nop_cnt); ++nop_i) __nop();  \
    }

#define POPCNT_U32(x) ((UWORD32)__popcnt((unsigned int)(x)))
#define PLD(a)
#define INLINE __forceinline

static INLINE UWORD32 CLZ(UWORD32 u4_word)
{
    unsigned long index;
    if(u4_word == 0)
        return 31;
    _BitScanReverse(&index, (unsigned long)u4_word);
    return (UWORD32)(31u - index);
}

static INLINE UWORD32 CLZNZ(UWORD32 u4_word)
{
    unsigned long index;
    _BitScanReverse(&index, (unsigned long)u4_word);
    return (UWORD32)(31u - index);
}

static INLINE UWORD32 CTZ(UWORD32 u4_word)
{
    unsigned long index;
    if(u4_word == 0)
        return 31;
    _BitScanForward(&index, (unsigned long)u4_word);
    return (UWORD32)index;
}

#define DATA_SYNC() _mm_mfence()

#define GET_POS_MSB_32(r, word)                      \
    {                                                \
        if(word)                                     \
        {                                            \
            unsigned long ihevc_msb_index;           \
            _BitScanReverse(&ihevc_msb_index,        \
                            (unsigned long)(word));   \
            (r) = (WORD32)ihevc_msb_index;           \
        }                                            \
        else                                         \
        {                                            \
            (r) = -1;                                \
        }                                            \
    }

#define GET_POS_MSB_64(r, word)                                \
    {                                                          \
        if(word)                                               \
        {                                                      \
            unsigned long ihevc_msb_index64;                   \
            _BitScanReverse64(&ihevc_msb_index64,              \
                              (unsigned __int64)(word));        \
            (r) = (WORD32)ihevc_msb_index64;                   \
        }                                                      \
        else                                                   \
        {                                                      \
            (r) = -1;                                          \
        }                                                      \
    }

#define GETRANGE(r, word)                               \
    {                                                   \
        if(word)                                        \
        {                                               \
            unsigned long ihevc_range_index;            \
            _BitScanReverse(&ihevc_range_index,         \
                            (unsigned long)(word));      \
            (r) = (WORD32)ihevc_range_index + 1;        \
        }                                               \
        else                                            \
        {                                               \
            (r) = 1;                                    \
        }                                               \
    }

#define GETRANGE64(r, llword)                                  \
    {                                                          \
        if(llword)                                             \
        {                                                      \
            unsigned long ihevc_range_index64;                 \
            _BitScanReverse64(&ihevc_range_index64,            \
                              (unsigned __int64)(llword));      \
            (r) = (WORD32)ihevc_range_index64 + 1;             \
        }                                                      \
        else                                                   \
        {                                                      \
            (r) = 1;                                           \
        }                                                      \
    }

#define GCC_ENABLE 0

#define PREFETCH_ENABLE 1
#if PREFETCH_ENABLE
#define PREFETCH(ptr, type) _mm_prefetch((const char *)(ptr), (type));
#else
#define PREFETCH(ptr, type)
#endif

#define MEM_ALIGN8 __declspec(align(8))
#define MEM_ALIGN16 __declspec(align(16))
#define MEM_ALIGN32 __declspec(align(32))

#endif /* _IHEVC_PLATFORM_MACROS_H_ */
