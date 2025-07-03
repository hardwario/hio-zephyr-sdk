#ifndef SUBSYS_HIO_ATCI_MODEM_TRACE_H_
#define SUBSYS_HIO_ATCI_MODEM_TRACE_H_

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

int hio_atci_modem_trace_process(const struct hio_atci *atci);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_ATCI_MODEM_TRACE_H_ */
