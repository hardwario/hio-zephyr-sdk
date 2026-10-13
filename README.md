# hio-zephyr-sdk

# Overview

This repository contains the necessary code and configuration to use the Hardwario Zephyr SDK within a Zephyr-based project.

# Dependencies

This repository depends on the [sdk-nrf](https://github.com/nrfconnect/sdk-nrf) repository for the necessary subsystems.

## Installation

Python dependencies of the SDK scripts (`west gen-codec`, `west serial-console`,
`cmake/app_codec.cmake`) are listed in `requirements.txt`:

```sh
pip install -r hio-zephyr-sdk/requirements.txt
```

## Supported versions

| Component | Version |
|-----------|---------|
| nRF Connect SDK (sdk-nrf) | `v3.4-branch` (primary, tested) |
| Zephyr SDK (toolchain) | 1.0.1 (see `zephyr/SDK_VERSION` in the NCS workspace) |
| Tested board | `nrf9151dk/nrf9151/ns` (samples), `native_sim` (tests) |

The `west.yml` of this repository intentionally does not pin sdk-nrf. The
application pins NCS in its own manifest and imports this module, for example:

```yaml
manifest:
  remotes:
    - name: ncs
      url-base: https://github.com/nrfconnect
    - name: hardwario
      url-base: https://github.com/hardwario
  projects:
    - name: sdk-nrf
      remote: ncs
      path: nrf
      revision: v3.4-branch   # prefer a fixed tag or SHA for releases
      import: true
    - name: hio-zephyr-sdk
      remote: hardwario
      revision: main
```

## Application requirements

- `hio_info` includes `<app_version.h>`, so the application must provide a
  [`VERSION`](https://docs.zephyrproject.org/latest/build/version/index.html)
  file next to its top-level `CMakeLists.txt`; otherwise the build fails with
  `app_version.h: No such file or directory`.
- SHA-256 for `hio_cloud` and the ATCI login defaults to PSA Crypto
  (`CONFIG_HIO_CLOUD_HASH_PSA`, `CONFIG_HIO_ATCI_LOGIN_HASH_PSA`). The mbedTLS
  legacy backend does not configure on NCS v3.4 and TinyCrypt is not shipped there.

-----------------------------------------------------------------------------------------------------------

# License

This project is licensed under the [MIT License](https://opensource.org/licenses/MIT/) - see the [LICENSE](https://github.com/hardwario/hio_zephyr_cloud/blob/main/LICENSE) file for details.
