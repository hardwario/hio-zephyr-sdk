/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef TEST_HIO_CLOUD_FAKE_BACKEND_H_
#define TEST_HIO_CLOUD_FAKE_BACKEND_H_

/* Zephyr includes */
#include <zephyr/kernel.h>

/* HIO includes */
#include <hio/hio_cloud.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HASH         0x0102030405060708ULL
#define ENCODER_HASH 0x1112131415161718ULL
#define HDR_LEN      9

#define FAKE_HEAD_LEN    64
#define FAKE_MAX_UPLINKS 4

/* Two-phase test_main(): suites run before and after hio_cloud_init() */
struct test_state {
	bool started;
};

bool phase_before_init(const void *state);
bool phase_started(const void *state);

struct fake_uplink {
	uint8_t head[FAKE_HEAD_LEN]; /* First bytes of the message */
	size_t len;                  /* Full message length */
	uint32_t crc;                /* CRC32 of the whole message */
	size_t buf_size;             /* Size of the buffer handed to the transport */
	k_timeout_t timeout;
	k_tid_t thread;
};

struct fake {
	/* Recorded by the fake transport */
	int uplink_count;
	struct fake_uplink uplinks[FAKE_MAX_UPLINKS];
	int downlink_count;
	k_tid_t downlink_thread;

	/* Set by the tests */
	int init_ret;
	int uplink_ret;
	bool uplink_has_downlink;
	int uplink_block; /* The next N uplinks wait for fake_release() */
};

extern struct fake g_fake;

/* Every uplink since the start, also the session request of the init work; unlike
 * g_fake, not cleared by fake_reset() */
#define FAKE_LOG_MAX 16

struct fake_log_entry {
	uint8_t head[FAKE_HEAD_LEN];
	size_t len;
	int ret;
};

int fake_log_count(void);
const struct fake_log_entry *fake_log(int index);
/* Index of the first logged uplink of message `type`, or -1 */
int fake_log_find(uint8_t type);
/* The first uplink of message `type` fails with `err`; not cleared by fake_reset() */
void fake_fail_first_of_type(uint8_t type, int err);

/* ---- Mock MCUboot image state and stored update id (src/mock_fw.c) ---- */

/* TEST_FW 0: no update id stored, 1: id stored and acked, 2: id stored, the ack fails,
 * 3: id stored, but the image is confirmed (MCUboot reverted the update) */
extern const uint8_t TEST_FW_UPDATE_ID[16];

/* Before hio_cloud_init(): the image state and the update id of TEST_FW */
void mock_fw_setup(void);
int mock_boot_confirm_calls(void);
bool mock_fw_update_id_stored(void);

/* Clears the records and the configuration */
void fake_reset(void);

/* Lets one blocked uplink continue */
void fake_release(void);

/* Waits until an uplink is blocked in the fake transport */
int fake_wait_uplink_blocked(k_timeout_t timeout);

/* Waits for a downlink */
int fake_wait_downlink(k_timeout_t timeout);

/* Lets the cloud work queue run whatever was submitted, for asserting that nothing happened */
void settle(void);

/* Asserts the message of uplink @p idx is the data header followed by @p payload */
void assert_data_msg(int idx, const uint8_t *payload, size_t len);

/* Asserts the whole message of uplink @p idx (not just its head) is the data header
 * followed by @p payload, by CRC32 */
void assert_data_msg_crc(int idx, const uint8_t *payload, size_t len);

/* Asserts the transfer timeout of uplink @p idx is within [lo_ms, hi_ms] */
void assert_timeout_in_ms(int idx, int lo_ms, int hi_ms);

/* Asserts the cloud lock can be taken from another thread */
void assert_lock_free(void);

/* Shared fixtures */

/* {1: 42} in an indefinite-length map */
extern const uint8_t g_payload[5];

/* {2: 3} */
extern const uint8_t g_payload2[3];

extern uint8_t g_big[HIO_CLOUD_TRANSFER_BUF_SIZE];

/* Common before() of the suites: resets the fake transport */
void fake_before(void *fixture);

/* Sends g_payload2 with a 10 ms timeout; a helper thread entry */
int send_payload2(void *arg);

/* Helper threads: index 0 and 1 (the slot after them is the lock probe) */
#define HELPER_COUNT 2

void helper_start(int idx, int (*fn)(void *arg), void *arg);

/* Joins the helper (aborted on timeout); returns its result or -EAGAIN on timeout */
int helper_join(int idx, k_timeout_t timeout, int *result);

#endif /* TEST_HIO_CLOUD_FAKE_BACKEND_H_ */
