#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test)

add_compile_definitions(CONFIG_HIO_CLOUD_LOG_LEVEL=3)
add_compile_definitions(CONFIG_HIO_CONFIG_LOG_LEVEL=3)
add_compile_definitions(CONFIG_HIO_CONFIG_INIT_PRIORITY=0)
add_compile_definitions(CONFIG_HIO_CONFIG_SETTINGS_PFX="")
add_compile_definitions(CONFIG_HIO_CONFIG_SETTINGS_KEY_MAX=96)

set(HIO_CLOUD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/hio_cloud)
set(HIO_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/hio_config)

include_directories(${HIO_CLOUD_DIR})
include_directories(${HIO_CONFIG_DIR})
target_sources(app PRIVATE ${HIO_CLOUD_DIR}/hio_cloud_msg.c)
target_sources(app PRIVATE ${HIO_CLOUD_DIR}/hio_cloud_util.c)
target_sources(app PRIVATE ${HIO_CONFIG_DIR}/hio_config.c)
target_sources(app PRIVATE ${HIO_CONFIG_DIR}/hio_config_shell.c)
target_sources(app PRIVATE src/stubs.c)
target_sources(app PRIVATE src/test_module.c)
target_sources(app PRIVATE src/test_hash.c)
target_sources(app PRIVATE src/test_pack_config.c)
