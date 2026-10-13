# Minimal application

Clean starting point for an application based on `hio-zephyr-sdk`.

## What it shows

- `west.yml` – the application pins NCS (`sdk-nrf` `v3.4-branch`) and imports
  `hio-zephyr-sdk`; the SDK itself does not pin NCS.
- `VERSION` – required by `hio_info` (`app_version.h`).
- `prj.conf` – PSA Crypto hash backends and the minimal TF-M profile.

## Standalone workspace

```sh
mkdir ws && cd ws
cp -r <path>/hio-zephyr-sdk/samples/minimal_app .
west init -l minimal_app
west update
west build -b nrf9151dk/nrf9151/ns minimal_app
```

Inside an existing NCS workspace that already has `hio-zephyr-sdk`:

```sh
west build -b nrf9151dk/nrf9151/ns hio-zephyr-sdk/samples/minimal_app
```
