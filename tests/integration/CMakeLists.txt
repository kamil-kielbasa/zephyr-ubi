# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_integration)

target_sources(app PRIVATE src/main.c)
