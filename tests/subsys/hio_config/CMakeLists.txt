#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test)

# hio_config sources are compiled directly (same pattern as tests/subsys/hio_lte);
# the Kconfig-derived macros the sources expect are provided here.
add_compile_definitions(CONFIG_HIO_CONFIG_LOG_LEVEL=3)
add_compile_definitions(CONFIG_HIO_CONFIG_INIT_PRIORITY=0)
add_compile_definitions(CONFIG_HIO_CONFIG_SETTINGS_PFX="")
add_compile_definitions(CONFIG_HIO_CONFIG_SETTINGS_KEY_MAX=96)

set(HIO_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/hio_config)

include_directories(${HIO_CONFIG_DIR})
target_sources(app PRIVATE ${HIO_CONFIG_DIR}/hio_config.c)
target_sources(app PRIVATE ${HIO_CONFIG_DIR}/hio_config_shell.c)
target_sources(app PRIVATE src/stubs.c)
target_sources(app PRIVATE src/test_module.c)
target_sources(app PRIVATE src/test_format_line.c)
target_sources(app PRIVATE src/test_parse_line.c)
target_sources(app PRIVATE src/test_show_parity.c)
target_sources(app PRIVATE src/main.c)
