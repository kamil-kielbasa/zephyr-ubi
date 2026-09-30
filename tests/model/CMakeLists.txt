# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_model)

include(${ZEPHYR_UBI_MODULE_DIR}/cmake/warnings.cmake)

target_sources(app PRIVATE
  ../common/forge.c
  ../common/partition.c
  ../common/suite.c
  ../common/table_copies.c
  src/model.c
  src/model_step.c
  src/test_model.c
)

# The flash simulator's faults and counters, and a device that refuses reads.
if(CONFIG_FLASH_SIMULATOR)
  target_sources(app PRIVATE
    ../common/flash_faults.c
    ../common/flash_shim.c
    ../common/flash_stats.c
  )
endif()

target_include_directories(app PRIVATE
  ${ZEPHYR_UBI_MODULE_DIR}/library
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

ubi_target_warnings(app)
