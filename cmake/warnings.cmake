# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

option(UBI_AUDIT_CONVERSIONS "Report implicit conversions without failing" OFF)

function(ubi_target_warnings target)
  set(base
    -Werror -Wall -Wextra
    -Walloca -Wdate-time -Wdouble-promotion -Wfloat-conversion -Wfloat-equal
    -Wformat-nonliteral -Wformat-security -Wformat-y2k -Wmissing-declarations
    -Wmissing-format-attribute -Wmissing-noreturn -Wmissing-prototypes
    -Wmultichar -Wnull-dereference -Wold-style-definition -Woverlength-strings
    -Wpointer-arith -Wshadow -Wswitch-default -Wswitch-enum -Wuninitialized
    -Wunreachable-code -Wunused-macros -Wwrite-strings)

  set(base_gcc
    -Waggregate-return -Walloc-zero -Warray-bounds=2 -Wattribute-alias=2
    -Wdisabled-optimization -Wduplicated-cond -Wformat-overflow=2
    -Wformat-signedness -Wformat-truncation=2 -Wimplicit-fallthrough=5
    -Winit-self -Wjump-misses-init -Wlogical-op -Wshift-overflow=2
    -Wstringop-overflow=4 -Wtrampolines -Wunused-const-variable=2)

  set(base_clang
    -Wno-unknown-warning-option
    -Warray-bounds-pointer-arithmetic -Wassign-enum -Wbad-function-cast
    -Wcast-function-type-strict -Wcomma -Wcompound-token-split
    -Wconditional-uninitialized -Wduplicate-enum -Wfour-char-constants
    -Wformat-non-iso -Wformat-pedantic -Widiomatic-parentheses
    -Wimplicit-fallthrough -Wloop-analysis -Wnewline-eof -Wredundant-parens
    -Wshadow-all -Wshift-sign-overflow -Wsigned-enum-bitfield
    -Wstatic-in-inline -Wstring-conversion -Wunreachable-code-aggressive
    -Wzero-length-array)

  if(UBI_AUDIT_CONVERSIONS)
    list(APPEND base -Wconversion -Wsign-conversion
                     -Wno-error=conversion -Wno-error=sign-conversion)
  endif()

  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${target} PRIVATE ${base} ${base_gcc})
  elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(${target} PRIVATE ${base} ${base_clang})
  endif()
endfunction()
