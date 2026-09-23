# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_integration)

include(${ZEPHYR_UBI_MODULE_DIR}/cmake/warnings.cmake)

target_sources(app PRIVATE
  ../common/common.c
  ../common/suite.c
  src/test_attach.c
  src/test_concurrency.c
  src/test_contract.c
  src/test_integrity.c
  src/test_leb.c
  src/test_maintenance.c
  src/test_trust.c
  src/test_volume.c
)

# Reads the flash simulator's operation counters.
if(CONFIG_FLASH_SIMULATOR)
  target_sources(app PRIVATE src/test_cost.c)
endif()

# The on-flash layout and key derivation, for tests that forge blocks.
target_include_directories(app PRIVATE
  ${ZEPHYR_UBI_MODULE_DIR}/library
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

ubi_target_warnings(app)
