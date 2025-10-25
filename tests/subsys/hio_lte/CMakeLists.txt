
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test)

add_compile_definitions(CONFIG_HIO_LTE_LOG_LEVEL=3)

set(HIO_LTE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/hio_lte)

include_directories(${HIO_LTE_DIR})
target_sources(app PRIVATE ${HIO_LTE_DIR}/hio_lte_parse.c)
target_sources(app PRIVATE src/test_parse.c)
