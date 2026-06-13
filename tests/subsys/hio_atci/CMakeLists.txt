#
# Copyright (c) 2025 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test)

# The hio_atci subsystem is built by the west module itself (it is enabled via
# CONFIG_HIO_ATCI=y in prj.conf). Unlike the hio_lte test we do NOT compile the
# subsystem sources manually here, so the module's iterable-section linker
# snippet (hio_atci.ld) is included as well.
target_sources(app PRIVATE src/main.c)
