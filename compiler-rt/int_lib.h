// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INT_LIB_H
#define INT_LIB_H

#define COMPILER_RT_ABI
#define compilerrt_abort() __builtin_trap()

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

typedef int32_t si_int;
typedef uint32_t su_int;
typedef int64_t di_int;
typedef uint64_t du_int;

typedef union {
  di_int all;
  struct {
    su_int low;
    si_int high;
  } s;
} dwords;

typedef union {
  du_int all;
  struct {
    su_int low;
    su_int high;
  } s;
} udwords;

#if defined(__SIZEOF_INT128__)
#define CRT_HAS_128BIT
typedef int ti_int __attribute__((mode(TI)));
typedef unsigned tu_int __attribute__((mode(TI)));

typedef union {
  ti_int all;
  struct {
    du_int low;
    di_int high;
  } s;
} twords;

typedef union {
  tu_int all;
  struct {
    du_int low;
    du_int high;
  } s;
} utwords;
#endif

#define CHAR_BIT 8
#define clzsi __builtin_clz
#define ctzsi __builtin_ctz

COMPILER_RT_ABI su_int __udivsi3(su_int a, su_int b);
COMPILER_RT_ABI si_int __divsi3(si_int a, si_int b);
COMPILER_RT_ABI si_int __modsi3(si_int a, si_int b);
COMPILER_RT_ABI su_int __udivmodsi4(su_int a, su_int b, su_int *rem);
COMPILER_RT_ABI si_int __divmodsi4(si_int a, si_int b, si_int *rem);
COMPILER_RT_ABI du_int __udivmoddi4(du_int a, du_int b, du_int *rem);
COMPILER_RT_ABI di_int __divmoddi4(di_int a, di_int b, di_int *rem);

#ifdef CRT_HAS_128BIT
COMPILER_RT_ABI tu_int __udivmodti4(tu_int a, tu_int b, tu_int *rem);
COMPILER_RT_ABI tu_int __udivti3(tu_int a, tu_int b);
COMPILER_RT_ABI ti_int __divti3(ti_int a, ti_int b);
COMPILER_RT_ABI ti_int __modti3(ti_int a, ti_int b);
COMPILER_RT_ABI ti_int __divmodti4(ti_int a, ti_int b, ti_int *rem);
#endif

#endif
