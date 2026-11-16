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
	CONFIG_HIO_CLOUD_ENCODE_ZCBOR_STATES=2
	CONFIG_HIO_LTE_SCAN_MAX_ENTRIES=10
	CONFIG_HIO_LTE_SCAN_MAX_CELLS=14
)

set(HIO_CLOUD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/hio_cloud)

# The real hio_cloud.c with a fake transport behind the backend seam
# (hio_cloud_backend_udp_lte), so sends go the real way into m_transfer_buf.
include_directories(${HIO_CLOUD_DIR})
target_sources(app PRIVATE
	${HIO_CLOUD_DIR}/hio_cloud.c
	${HIO_CLOUD_DIR}/hio_cloud_msg_data.c
	${HIO_CLOUD_DIR}/hio_cloud_cbor.c
	src/main.c
	src/fake_backend.c
	src/stubs.c
	src/test_send_data.c
	src/test_send_data_cb.c
)
