# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

if(CONFIG_UBI)

  zephyr_include_directories(include)

  zephyr_library_named(ubi)
  zephyr_library_include_directories(library)

  zephyr_library_sources(
    library/ubi_header.c
    library/ubi_key.c
  )

endif()
