/*
 * Force-included (-include openmw3ds_int32.h) into every C/C++ translation unit of the port and of
 * the C++ dependencies we build ourselves.
 *
 * arm-none-eabi GCC defines int32_t as `long`. Everywhere OpenMW and its dependencies are developed
 * (glibc/musl/MSVC/Apple/Android, and the Vita toolchain) int32_t is `int`, and code relies on it:
 * ICU 73 has ~70 overload/override errors otherwise (std::max(int, int32_t), virtuals declared with
 * int and overridden with int32_t, ...). Both are 32-bit with the same calling convention, so prebuilt
 * C libraries (newlib, libctru, citro3d, portlibs) are unaffected; only C++ name mangling changes,
 * which is why every C++ library is rebuilt with this header.
 * newlib's <stdint.h>/<inttypes.h> derive their types and PRI macros from these macros.
 */
#ifndef OPENMW3DS_INT32_H
#define OPENMW3DS_INT32_H

#undef __INT32_TYPE__
#define __INT32_TYPE__ int
#undef __UINT32_TYPE__
#define __UINT32_TYPE__ unsigned int
#undef __INT_LEAST32_TYPE__
#define __INT_LEAST32_TYPE__ int
#undef __UINT_LEAST32_TYPE__
#define __UINT_LEAST32_TYPE__ unsigned int
#undef __INT32_C
#define __INT32_C(c) c
#undef __UINT32_C
#define __UINT32_C(c) c##U
#undef __INT32_MAX__
#define __INT32_MAX__ 0x7fffffff
#undef __UINT32_MAX__
#define __UINT32_MAX__ 0xffffffffU
#undef __INT_LEAST32_MAX__
#define __INT_LEAST32_MAX__ 0x7fffffff
#undef __UINT_LEAST32_MAX__
#define __UINT_LEAST32_MAX__ 0xffffffffU

#endif
