# Copyright (c) 2026, Kamil Kiełbasa and others
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_unit)

target_sources(app PRIVATE
  src/common.c
  src/suite.c
  src/test_header.c
  src/test_key.c
  src/test_mac.c
)

# Header serialization and key derivation have no public API of their own, so
# the unit tests reach them through the library's internal header. No test
# hooks are added to the library itself.
target_include_directories(app PRIVATE ${ZEPHYR_UBI_MODULE_DIR}/library)
