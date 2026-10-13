zephyr_library()

zephyr_library_sources(hio_nvs.c)
zephyr_library_sources_ifdef(CONFIG_HIO_NVS_SHELL hio_nvs_shell.c)
