# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

# =============================================================================
# ubi_target_warnings(<target>)
#
# Diagnostics for the library's own translation units. All flags are PRIVATE,
# so nothing here reaches an application that merely uses UBI.
#
# The set is the one libedhoc uses, minus what Zephyr's headers cannot survive.
# Those headers arrive through -I rather than -isystem, so their static inline
# functions are diagnosed as if they were ours:
#
#   -Wstrict-prototypes  cbprintf.h, which every LOG_ call pulls in, declares
#                  a function without one.
#   -Wundef        Kconfig booleans are tested with `#if CONFIG_FOO`, and an
#                  unset boolean is simply not defined.
#   -Wconversion   sys_put_le64(), k_uptime_get() and crc32_k_4_2_update() all
#   -Wsign-...     convert in ways GCC reports. Turn UBI_AUDIT_CONVERSIONS on
#                  to read them; they cannot be errors.
# =============================================================================

option(UBI_AUDIT_CONVERSIONS
       "Warn about implicit conversions, Zephyr's own headers included" OFF)

function(ubi_target_warnings target)
  set(base
    -Werror -Wall -Wextra
    -Wcast-align -Wdouble-promotion
    -Wformat=2 -Wunreachable-code
    -Wmissing-prototypes -Wold-style-definition
    -Wshadow -Wpointer-arith -Wuninitialized
    -Wnull-dereference -Wswitch-enum -Wswitch-default)

  set(base_gcc
    -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Winit-self
    -Wjump-misses-init)

  if(UBI_AUDIT_CONVERSIONS)
    list(APPEND base -Wconversion -Wsign-conversion
                     -Wno-error=conversion -Wno-error=sign-conversion)
  endif()

  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${target} PRIVATE ${base} ${base_gcc})
  elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(${target} PRIVATE ${base})
  endif()
endfunction()
