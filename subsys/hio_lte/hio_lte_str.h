#ifndef SUBSYS_HIO_LTE_STR_H_
#define SUBSYS_HIO_LTE_STR_H_

/* HIO includes */
#include <hio/hio_lte.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Convert event code to string. */
const char *hio_lte_str_fsm_event(enum hio_lte_fsm_event event);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_STR_H_ */
