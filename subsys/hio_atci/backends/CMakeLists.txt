#
# Copyright (c) 2025 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#

zephyr_library()

zephyr_sources_ifdef(CONFIG_HIO_ATCI_BACKEND_UART hio_atci_uart.c)
