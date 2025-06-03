#ifndef SUBSYS_HIO_ATCI_IO_H_
#define SUBSYS_HIO_ATCI_IO_H_

/* HIO includes */
#include <hio/hio_atci.h>

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void hio_atci_io_write(const struct hio_atci *atci, const void *data, size_t length);
void hio_atci_io_endline(const struct hio_atci *atci);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_ATCI_IO_H_ */
