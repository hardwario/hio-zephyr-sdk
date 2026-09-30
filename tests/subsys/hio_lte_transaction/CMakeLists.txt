
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test)

# The hio_lte Kconfig is not part of this test build, so supply the symbols
# hio_lte.c needs at compile time (defaults taken from subsys/hio_lte/Kconfig).
add_compile_definitions(CONFIG_HIO_LTE_LOG_LEVEL=3)
add_compile_definitions(CONFIG_HIO_LTE_INIT_PRIORITY=99)
add_compile_definitions(CONFIG_HIO_LTE_SCAN_MAX_ENTRIES=10)
add_compile_definitions(CONFIG_HIO_LTE_SCAN_MAX_CELLS=14)
add_compile_definitions(CONFIG_HIO_LTE_CEREG_HISTORY=3)
add_compile_definitions(CONFIG_HIO_LTE_SCAN_AUTO=1)
add_compile_definitions(CONFIG_HIO_LTE_SCAN_AUTO_EVERY=1)

set(HIO_LTE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/hio_lte)

include_directories(${HIO_LTE_DIR})

# hio_lte_talk.h (pulled in by hio_lte.c) includes nrf_modem_at.h. Only the
# declarations are needed here — the modem library itself is faked away — so put
# nrfxlib's headers on the path rather than stubbing them.
include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)
target_sources(app PRIVATE ${HIO_LTE_DIR}/hio_lte.c)
target_sources(app PRIVATE ${HIO_LTE_DIR}/hio_lte_parse.c)
target_sources(app PRIVATE ${HIO_LTE_DIR}/hio_lte_state.c)
target_sources(app PRIVATE ${HIO_LTE_DIR}/hio_lte_str.c)
target_sources(app PRIVATE ${HIO_LTE_DIR}/hio_lte_util.c)
target_sources(app PRIVATE src/fake_flow.c)
target_sources(app PRIVATE src/test_transaction.c)
target_sources(app PRIVATE src/test_scan.c)
