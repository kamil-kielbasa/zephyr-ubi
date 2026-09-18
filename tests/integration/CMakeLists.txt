# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_integration)

target_sources(app PRIVATE
  src/common.c
  src/suite.c
  src/test_ageing.c
  src/test_attach.c
  src/test_integrity.c
  src/test_leb.c
  src/test_maintenance.c
  src/test_trust.c
  src/test_volume.c
)
