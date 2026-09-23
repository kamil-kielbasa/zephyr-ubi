# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_ageing)

include(${ZEPHYR_UBI_MODULE_DIR}/cmake/warnings.cmake)

target_sources(app PRIVATE
  ../common/common.c
  ../common/suite.c
  src/test_ageing.c
)

target_include_directories(app PRIVATE
  ${ZEPHYR_UBI_MODULE_DIR}/library
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

ubi_target_warnings(app)
