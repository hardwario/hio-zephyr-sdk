#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
#
# Generate app_codec.h (CBOR keys, decoder/encoder buffers and their hashes
# for hio_cloud_options) from the application's codec YAML files.
#
# Usage in the application CMakeLists.txt, after find_package(Zephyr):
#
#   include(${ZEPHYR_HIO_ZEPHYR_SDK_MODULE_DIR}/cmake/app_codec.cmake)
#
# Inputs (relative to the application source directory):
#   codec/cbor-decoder.yaml  required
#   codec/cbor-encoder.yaml  optional
#
# The application then does `#include <app_codec.h>`.
# Requires the Python packages listed in requirements.txt (cbor2, PyYAML).

include_guard()

if(NOT DEFINED ZEPHYR_HIO_ZEPHYR_SDK_MODULE_DIR)
	message(FATAL_ERROR "app_codec.cmake: hio-zephyr-sdk module not found")
endif()

set(APP_CODEC_DIR ${PROJECT_BINARY_DIR}/include/generated/app)
set(APP_CODEC_DECODER ${CMAKE_CURRENT_SOURCE_DIR}/codec/cbor-decoder.yaml)
set(APP_CODEC_ENCODER ${CMAKE_CURRENT_SOURCE_DIR}/codec/cbor-encoder.yaml)
set(APP_CODEC_GEN ${ZEPHYR_HIO_ZEPHYR_SDK_MODULE_DIR}/scripts/west_commands/gen-codec.py)

if(NOT EXISTS ${APP_CODEC_DECODER})
	message(FATAL_ERROR "app_codec.cmake: missing ${APP_CODEC_DECODER}")
endif()

set(APP_CODEC_DEPS ${APP_CODEC_DECODER} ${APP_CODEC_GEN})

if(EXISTS ${APP_CODEC_ENCODER})
	list(APPEND APP_CODEC_DEPS ${APP_CODEC_ENCODER})
else()
	set(APP_CODEC_ENCODER "")
endif()

# Same output as `west gen-codec`, but the generator functions are called
# directly so the build does not depend on west command registration (e.g.
# when hio-zephyr-sdk is added via EXTRA_ZEPHYR_MODULES).
add_custom_command(
	OUTPUT ${APP_CODEC_DIR}/app_codec.h
	COMMAND ${CMAKE_COMMAND} -E make_directory ${APP_CODEC_DIR}
	COMMAND ${PYTHON_EXECUTABLE} -c
		"import runpy, sys; g = runpy.run_path(sys.argv[1]); load = lambda p: g['load_yaml'](p) if p else None; g['write_h'](sys.argv[4], {'decoder': load(sys.argv[2]), 'encoder': load(sys.argv[3])})"
		${APP_CODEC_GEN} ${APP_CODEC_DECODER} "${APP_CODEC_ENCODER}" ${APP_CODEC_DIR}/app_codec.h
	DEPENDS ${APP_CODEC_DEPS}
	VERBATIM
)

add_custom_target(app_codec_h DEPENDS ${APP_CODEC_DIR}/app_codec.h)

zephyr_include_directories(${APP_CODEC_DIR})
add_dependencies(zephyr_generated_headers app_codec_h)
