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
add_compile_definitions(CONFIG_HIO_CLOUD_LOG_LEVEL=4)
if(NOT DEFINED TEST_ZCBOR_STATES)
  set(TEST_ZCBOR_STATES 2)
endif()
add_compile_definitions(CONFIG_HIO_CLOUD_ENCODE_ZCBOR_STATES=${TEST_ZCBOR_STATES})
add_compile_definitions(CONFIG_HIO_LTE_SCAN_MAX_ENTRIES=10)
add_compile_definitions(CONFIG_HIO_LTE_SCAN_MAX_CELLS=14)
add_compile_definitions(CONFIG_HIO_LTE_LOG_LEVEL=3)

set(HIO_CLOUD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/hio_cloud)
# The NCELLMEAS parser feeds the encoder test
set(HIO_LTE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/hio_lte)

include_directories(${HIO_CLOUD_DIR})
include_directories(${HIO_LTE_DIR})
target_sources(app PRIVATE ${HIO_CLOUD_DIR}/hio_cloud_msg_data.c)
target_sources(app PRIVATE ${HIO_CLOUD_DIR}/hio_cloud_cbor.c)
target_sources(app PRIVATE ${HIO_LTE_DIR}/hio_lte_parse.c)
target_sources(app PRIVATE src/test_pack_data.c)
target_sources(app PRIVATE src/test_cbor_ncellmeas.c)
