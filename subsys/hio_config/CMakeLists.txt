zephyr_library()

zephyr_library_sources_ifdef(CONFIG_HIO_CONFIG hio_config.c)
