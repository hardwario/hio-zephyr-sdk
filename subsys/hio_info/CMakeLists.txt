#
# Copyright (c) 2025 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#

zephyr_library()

if(DEFINED ENV{FW_BUNDLE})
  add_definitions(-DFW_BUNDLE=$ENV{FW_BUNDLE})
endif()

if(DEFINED ENV{FW_NAME})
  add_definitions(-DFW_NAME=$ENV{FW_NAME})
endif()

zephyr_library_sources(hio_info.c)
zephyr_library_sources_ifdef(CONFIG_HIO_INFO_BLE hio_info_ble.c)
zephyr_library_sources_ifdef(CONFIG_HIO_INFO_SHELL hio_info_shell.c)
zephyr_library_sources_ifdef(CONFIG_HIO_INFO_ATCI hio_info_atci.c)
