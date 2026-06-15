zephyr_library()

# Two fully independent implementations sharing only <hio/hio_rtc.h>.
# Pick one based on the target SoC series.
if(CONFIG_SOC_SERIES_NRF54LX)
  zephyr_library_sources(hio_rtc_nrf54.c)
else()
  zephyr_library_sources(hio_rtc_nrf5x.c)
endif()
