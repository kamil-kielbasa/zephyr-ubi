# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

if(CONFIG_UBI)

  include(${CMAKE_CURRENT_LIST_DIR}/cmake/warnings.cmake)

  zephyr_include_directories(include)

  zephyr_library_named(ubi)
  zephyr_library_include_directories(library)

  zephyr_library_sources(
    library/ubi_api.c
    library/ubi_device.c
    library/ubi_header.c
    library/ubi_io.c
    library/ubi_key.c
    library/ubi_peb.c
    library/ubi_state.c
    library/ubi_volume.c
    library/ubi_volume_table.c
  )

  ubi_target_warnings(ubi)

endif()
