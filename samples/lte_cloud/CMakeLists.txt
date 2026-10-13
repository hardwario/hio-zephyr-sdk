#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(application)

# Generates app_codec.h from codec/cbor-decoder.yaml
include(${ZEPHYR_HIO_ZEPHYR_SDK_MODULE_DIR}/cmake/app_codec.cmake)

target_sources(app PRIVATE src/main.c)
