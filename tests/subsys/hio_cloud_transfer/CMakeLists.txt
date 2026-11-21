#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test)

# Kconfig of the hio_cloud subsystem is not enabled here (sources are compiled
# manually), so the symbols the sources need are defined on the command line.
add_compile_definitions(
	CONFIG_HIO_CLOUD_LOG_LEVEL=3
	CONFIG_HIO_CLOUD_HASH_PSA=1
	CONFIG_HIO_CLOUD_TRANSFER_BUF_SIZE=16384
	CONFIG_HIO_LTE_SCAN_MAX_ENTRIES=10
	CONFIG_HIO_LTE_SCAN_MAX_CELLS=14
)

set(HIO_CLOUD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/hio_cloud)

# The real hio_cloud_transfer.c over a scripted hio_lte_send_recv()
include_directories(${HIO_CLOUD_DIR})
target_sources(app PRIVATE
	${HIO_CLOUD_DIR}/hio_cloud_packet.c
	${HIO_CLOUD_DIR}/hio_cloud_transfer.c
	src/mock_hio_lte.c
	src/test_transfer.c
)
