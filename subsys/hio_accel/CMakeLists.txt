#
# Copyright (c) 2023 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#

zephyr_library()

zephyr_library_sources(hio_accel.c)
zephyr_library_sources_ifdef(CONFIG_HIO_ACCEL_SHELL hio_accel_shell.c)
