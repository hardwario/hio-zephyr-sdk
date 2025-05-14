#
# Copyright (c) 2025 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#

zephyr_library()

add_subdirectory(backends)

zephyr_library_sources(hio_atci.c)
zephyr_library_sources(hio_atci_cmd.c)

zephyr_linker_sources(ROM_SECTIONS hio_atci.ld)
