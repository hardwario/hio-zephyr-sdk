#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_state.h"
#include "hio_lte_str.h"
#include "hio_lte_talk.h"

/* HIO includes */
#include <hio/hio_rtc.h>
#include <hio/hio_lte.h>

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/time_units.h>
#include <zephyr/sys/ring_buffer.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_lte, CONFIG_HIO_LTE_LOG_LEVEL);

#define SIMDETECTED_TIMEOUT    K_SECONDS(10)
#define MDMEV_RESET_LOOP_DELAY K_MINUTES(32)
#define SEND_CSCON_1_TIMEOUT   K_SECONDS(30)
#define CONEVAL_TIMEOUT        K_SECONDS(30)
#define NCELLMEAS_TIMEOUT      K_SECONDS(60)

#define WORK_Q_STACK_SIZE 4096
#define WORK_Q_PRIORITY   K_LOWEST_APPLICATION_THREAD_PRIO

static struct k_work_q m_work_q;
static K_THREAD_STACK_DEFINE(m_work_q_stack, WORK_Q_STACK_SIZE);

enum fsm_state {
	FSM_STATE_DISABLED = 0,
	FSM_STATE_ERROR,
	FSM_STATE_PREPARE,
	FSM_STATE_ATTACH,
	FSM_STATE_RETRY_DELAY,
	FSM_STATE_RESET_LOOP,
	FSM_STATE_OPEN_SOCKET,
	FSM_STATE_READY,
	FSM_STATE_SLEEP,
	FSM_STATE_SEND,
	FSM_STATE_RECEIVE,
	FSM_STATE_CONEVAL,
	FSM_STATE_NCELLMEAS,
};

struct fsm_state_desc {
	enum fsm_state state;
	int (*on_enter)(void);
	int (*on_leave)(void);
	int (*event_handler)(enum hio_lte_fsm_event event);
};

static struct hio_lte_socket_config m_socket_config = {0};
int m_attach_retry_count = 0;
enum fsm_state m_state;
K_MUTEX_DEFINE(m_state_lock);
struct k_work_delayable m_timeout_work;
struct k_work m_event_dispatch_work;
uint8_t m_event_buf[8];
struct ring_buf m_event_rb;
K_MUTEX_DEFINE(m_event_rb_lock);

static K_EVENT_DEFINE(m_states_event);
#define SEND_RECV_BIT BIT(0)
#define ATTACHED_BIT  BIT(1)
#define CONNECTED_BIT BIT(2)

#define FLAG_CSCON         BIT(0)
#define FLAG_GNSS_ENABLE   BIT(1)
#define FLAG_CFUN4         BIT(2)
#define FLAG_NCELLMEAS_REQ BIT(3)
#define FLAG_DTLS_SAVED    BIT(4)
atomic_t m_flag = ATOMIC_INIT(0);

K_MUTEX_DEFINE(m_send_recv_lock);
struct hio_lte_send_recv_param *m_send_recv_param = NULL;

struct hio_lte_metrics m_metrics;
K_MUTEX_DEFINE(m_metrics_lock);
static uint32_t m_start = 0;
static uint32_t m_start_cscon1 = 0;

struct hio_lte_cereg_param m_cereg_param;

static sys_slist_t cb_list = SYS_SLIST_STATIC_INIT(&cb_list);

#define ON_ERROR_MAX_FLOW_CHECK_RETRIES 3
static struct {
	enum fsm_state prev_state;
	int flow_check_failures;
	enum fsm_state on_timeout_state;
	uint32_t timeout_s;
} m_error_ctx = {0};

static struct fsm_state_desc *get_fsm_state(enum fsm_state state);

static struct hio_lte_attach_timeout get_attach_timeout(int attempt)
{
	switch (g_hio_lte_config.attach_policy) {
	case HIO_LTE_ATTACH_POLICY_AGGRESSIVE:
		return hio_lte_flow_attach_policy_periodic(attempt, K_NO_WAIT);
	case HIO_LTE_ATTACH_POLICY_PERIODIC_2H:
		return hio_lte_flow_attach_policy_periodic(attempt, K_HOURS(1));
	case HIO_LTE_ATTACH_POLICY_PERIODIC_6H:
		return hio_lte_flow_attach_policy_periodic(attempt, K_HOURS(5));
	case HIO_LTE_ATTACH_POLICY_PERIODIC_12H:
		return hio_lte_flow_attach_policy_periodic(attempt, K_HOURS(11));
	case HIO_LTE_ATTACH_POLICY_PERIODIC_1D:
		return hio_lte_flow_attach_policy_periodic(attempt, K_HOURS(23));
	case HIO_LTE_ATTACH_POLICY_PROGRESSIVE:
		return hio_lte_flow_attach_policy_progressive(attempt);
	default:
		return hio_lte_flow_attach_policy_periodic(attempt, K_HOURS(1));
	}
}

int hio_lte_get_curr_attach_info(int *attempt, int *attach_timeout_sec, int *retry_delay_sec,
				 int *remaining_sec)
{
	struct hio_lte_attach_timeout timeout = get_attach_timeout(m_attach_retry_count);
	if (attempt) {
		*attempt = m_attach_retry_count + 1;
	}
	if (attach_timeout_sec) {
		*attach_timeout_sec = k_ticks_to_sec_floor32(timeout.attach_timeout.ticks);
	}
	if (retry_delay_sec) {
		*retry_delay_sec = k_ticks_to_sec_floor32(timeout.retry_delay.ticks);
	}
	if (remaining_sec) {
		k_ticks_t ticks = k_work_delayable_remaining_get(&m_timeout_work);
		*remaining_sec = k_ticks_to_sec_floor32(ticks);
	}
	return 0;
}

int hio_lte_get_socket_mtu(size_t *mtu)
{
	*mtu = HIO_LTE_UDP_MAX_MTU;

	if (m_socket_config.dtls_enabled) {
		int cipher;
		hio_lte_get_dtls_ciphersuite_used(&cipher);
		if (cipher == 0xc0a8) { /* MBEDTLS_TLS_PSK_WITH_AES_128_CCM_8 */
			*mtu -= (HIO_LTE_DTLS_HEADERS_SIZE + 16);
		} else {
			*mtu -= (HIO_LTE_DTLS_HEADERS_SIZE + 60); /* default overhead */
		}
	}

	return 0;
}

static int set_psk(const char *identity, const char *psk_hex)
{
	if (!identity || !psk_hex) {
		return -EINVAL;
	}

	if (strlen(identity) == 0) {
		LOG_ERR("PSK identity is empty");
		return -EINVAL;
	}

	size_t psk_len = strlen(psk_hex);
	if (psk_len == 0 || (psk_len % 2) != 0) {
		LOG_ERR("PSK hex string is invalid");
		return -EINVAL;
	}

	for (size_t i = 0; i < psk_len; i++) {
		char c = psk_hex[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			LOG_ERR("PSK hex string contains non-hex character");
			return -EINVAL;
		}
	}

	int ret;
	if (m_state == FSM_STATE_DISABLED) {
		ret = hio_lte_flow_start();
		if (ret < 0) {
			LOG_ERR("Problem starting LTE flow failed: %d", ret);
			return ret;
		}
	}

	ret = hio_lte_flow_set_psk(identity, psk_hex);
	if (ret < 0) {
		LOG_ERR("Problem setting PSK failed: %d", ret);
	}

	if (m_state == FSM_STATE_DISABLED) {
		int err = hio_lte_flow_stop();
		if (err < 0) {
			LOG_ERR("Problem stopping LTE flow: %d", err);
		}
	}

	return ret;
}

int hio_lte_set_psk(const char *identity, const char *psk_hex)
{
	k_mutex_lock(&m_state_lock, K_FOREVER);
	int ret = set_psk(identity, psk_hex);
	k_mutex_unlock(&m_state_lock);
	return ret;
}

const char *fsm_state_str(enum fsm_state state)
{
	switch (state) {
	case FSM_STATE_DISABLED:
		return "disabled";
	case FSM_STATE_ERROR:
		return "error";
	case FSM_STATE_PREPARE:
		return "prepare";
	case FSM_STATE_RESET_LOOP:
		return "reset_loop";
	case FSM_STATE_RETRY_DELAY:
		return "retry_delay";
	case FSM_STATE_ATTACH:
		return "attach";
	case FSM_STATE_OPEN_SOCKET:
		return "open_socket";
	case FSM_STATE_READY:
		return "ready";
	case FSM_STATE_SLEEP:
		return "sleep";
	case FSM_STATE_SEND:
		return "send";
	case FSM_STATE_RECEIVE:
		return "receive";
	case FSM_STATE_CONEVAL:
		return "coneval";
	case FSM_STATE_NCELLMEAS:
		return "ncellmeas";
	}
	return "unknown";
}

static void start_timer(k_timeout_t timeout)
{
	k_work_schedule(&m_timeout_work, timeout);
}

static void stop_timer(void)
{
	k_work_cancel_delayable(&m_timeout_work);
}

static void delegate_event(enum hio_lte_fsm_event event)
{
	if (event == HIO_LTE_FSM_EVENT_CSCON_1) {
		atomic_set_bit(&m_flag, FLAG_CSCON);
		m_start_cscon1 = k_uptime_get_32();
	} else if (event == HIO_LTE_FSM_EVENT_CSCON_0) {
		atomic_clear_bit(&m_flag, FLAG_CSCON);
		k_mutex_lock(&m_metrics_lock, K_FOREVER);
		m_metrics.cscon_1_last_duration_ms = k_uptime_get_32() - m_start_cscon1;
		m_metrics.cscon_1_duration_ms += m_metrics.cscon_1_last_duration_ms;
		k_mutex_unlock(&m_metrics_lock);
	}

	k_mutex_lock(&m_event_rb_lock, K_FOREVER);
	int ret = ring_buf_put(&m_event_rb, (uint8_t *)&event, 1);
	k_mutex_unlock(&m_event_rb_lock);

	if (ret < 0) {
		LOG_WRN("Failed to put event in ring buffer");
		return;
	}

	ret = k_work_submit_to_queue(&m_work_q, &m_event_dispatch_work);
	if (ret < 0) {
		LOG_WRN("Failed to submit work to queue");
	}
}

static void hio_lte_notify(enum hio_lte_event event)
{
	struct hio_lte_cb *cb;

	SYS_SLIST_FOR_EACH_CONTAINER(&cb_list, cb, node) {
		cb->handler(cb, event);
	}
}

static void event_handler(enum hio_lte_fsm_event event)
{
	LOG_INF("event: %s, state: %s", hio_lte_str_fsm_event(event), fsm_state_str(m_state));

	switch (event) {
	case HIO_LTE_FSM_EVENT_CSCON_0:
		hio_lte_notify(HIO_LTE_EVENT_CSCON_0);
		break;
	case HIO_LTE_FSM_EVENT_CSCON_1:
		hio_lte_notify(HIO_LTE_EVENT_CSCON_1);
		break;
	default:
		break;
	}

	struct fsm_state_desc *fsm_state = get_fsm_state(m_state);
	if (fsm_state && fsm_state->event_handler) {
		int ret = fsm_state->event_handler(event);
		if (ret < 0) {
			stop_timer();
			LOG_WRN("failed to handle event, error: %i", ret);
			if (event != HIO_LTE_FSM_EVENT_ERROR) {
				delegate_event(HIO_LTE_FSM_EVENT_ERROR);
			}
		}
	}
}

static inline int leaving_state(enum fsm_state next)
{
	struct fsm_state_desc *fsm_state = get_fsm_state(m_state);
	if (fsm_state && fsm_state->on_leave) {
		LOG_INF("%s", fsm_state_str(m_state));
		int ret = fsm_state->on_leave();
		if (ret < 0) {
			LOG_WRN("failed to leave state, error: %i", ret);
			if (next != FSM_STATE_ERROR) {
				delegate_event(HIO_LTE_FSM_EVENT_ERROR);
			}
			return ret;
		}
	}
	return 0;
}

static void entering_state(struct fsm_state_desc *next_desc)
{
	LOG_INF("%s", fsm_state_str(next_desc->state));

	k_mutex_lock(&m_state_lock, K_FOREVER);
	m_state = next_desc->state;
	k_mutex_unlock(&m_state_lock);
	if (next_desc->on_enter) {
		int ret = next_desc->on_enter();
		if (ret < 0) {
			LOG_WRN("failed to enter state error: %i", ret);
			if (next_desc->state != FSM_STATE_ERROR) {
				delegate_event(HIO_LTE_FSM_EVENT_ERROR);
			}
			return;
		}
	}
}

static void transition_state(enum fsm_state next)
{
	k_mutex_lock(&m_state_lock, K_FOREVER);
	enum fsm_state current = m_state;
	k_mutex_unlock(&m_state_lock);

	if (next == current) {
		LOG_INF("no-op: already in %s", fsm_state_str(next));
		return;
	}

	struct fsm_state_desc *next_desc = get_fsm_state(next);
	if (!next_desc) {
		LOG_ERR("Unknown state: %s %d", fsm_state_str(next), next);
		return;
	}

	if (next == FSM_STATE_ERROR && current == FSM_STATE_ERROR) {
		LOG_WRN("Already in error state, ignoring");
		return;
	}

	if (current == FSM_STATE_ERROR) {
		m_error_ctx.prev_state = current; /* Save previous state before error */
	}

	if (leaving_state(next) == 0) {
		entering_state(next_desc);
	}
}

static void timeout_work_handler(struct k_work *item)
{
	delegate_event(HIO_LTE_FSM_EVENT_TIMEOUT);
}

static void event_dispatch_work_handler(struct k_work *item)
{
	uint8_t events[sizeof(m_event_buf)];
	uint8_t events_cnt;

	k_mutex_lock(&m_event_rb_lock, K_FOREVER);
	events_cnt = (uint8_t)ring_buf_get(&m_event_rb, events, ARRAY_SIZE(events));
	k_mutex_unlock(&m_event_rb_lock);

	for (uint8_t i = 0; i < events_cnt; i++) {
		event_handler((enum hio_lte_fsm_event)events[i]);
	}
}

int hio_lte_enable(const struct hio_lte_socket_config *socket_config)
{
	if (!socket_config) {
		return -EINVAL;
	}
	memcpy(&m_socket_config, socket_config, sizeof(struct hio_lte_socket_config));
	if (g_hio_lte_config.test) {
		LOG_WRN("LTE Test mode enabled");
		return -ENOTSUP;
	}
	delegate_event(HIO_LTE_FSM_EVENT_ENABLE);
	return 0;
}

int hio_lte_reconnect(void)
{
	if (g_hio_lte_config.test) {
		LOG_WRN("LTE Test mode enabled");
		return -ENOTSUP;
	}

	k_mutex_lock(&m_state_lock, K_FOREVER);
	enum fsm_state current = m_state;
	k_mutex_unlock(&m_state_lock);

	if (current == FSM_STATE_DISABLED) {
		LOG_WRN("Cannot reconnect, LTE is disabled");
		return -ENODEV;
	}

	stop_timer();

	k_work_cancel(&m_event_dispatch_work);

	k_mutex_lock(&m_event_rb_lock, K_FOREVER);
	ring_buf_reset(&m_event_rb);
	k_mutex_unlock(&m_event_rb_lock);

	transition_state(FSM_STATE_DISABLED);

	delegate_event(HIO_LTE_FSM_EVENT_ENABLE);

	return 0;
}

int hio_lte_is_attached(bool *attached)
{
	*attached = k_event_test(&m_states_event, ATTACHED_BIT) ? true : false;
	return 0;
}

int hio_lte_wait_for_connected(k_timeout_t timeout)
{
	if (k_event_wait(&m_states_event, CONNECTED_BIT, false, timeout)) {
		return 0;
	}
	return -ETIMEDOUT;
}

int hio_lte_get_imei(uint64_t *imei)
{
	return hio_lte_state_get_imei(imei);
}

int hio_lte_get_imsi(uint64_t *imsi)
{
	return hio_lte_state_get_imsi(imsi);
}

int hio_lte_get_iccid(char **iccid)
{
	return hio_lte_state_get_iccid(iccid);
}

int hio_lte_get_modem_fw_version(char **version)
{
	return hio_lte_state_get_modem_fw_version(version);
}

int hio_lte_send_recv(const struct hio_lte_send_recv_param *param)
{
	LOG_INF("send_len: %u", param->send_len);

	k_timepoint_t end = sys_timepoint_calc(param->timeout);

	k_mutex_lock(&m_send_recv_lock, sys_timepoint_timeout(end));

	LOG_DBG("locked");

	m_send_recv_param = (struct hio_lte_send_recv_param *)param;

	k_event_clear(&m_states_event, SEND_RECV_BIT);

	delegate_event(HIO_LTE_FSM_EVENT_SEND);

	LOG_DBG("waiting for end transaction");

	k_event_wait(&m_states_event, SEND_RECV_BIT, false, sys_timepoint_timeout(end));

	if (sys_timepoint_expired(end)) {
		k_mutex_unlock(&m_send_recv_lock);
		delegate_event(HIO_LTE_FSM_EVENT_TIMEOUT);
		return -ETIMEDOUT;
	}

	k_mutex_unlock(&m_send_recv_lock);

	LOG_DBG("unlock");

	return 0;
}

int hio_lte_get_conn_param(struct hio_lte_conn_param *param)
{
	return hio_lte_state_get_conn_param(param);
}

int hio_lte_get_cereg_param(struct hio_lte_cereg_param *param)
{
	return hio_lte_state_get_cereg_param(param);
}

int hio_lte_get_metrics(struct hio_lte_metrics *metrics)
{
	if (!metrics) {
		return -EINVAL;
	}
	k_mutex_lock(&m_metrics_lock, K_FOREVER);
	memcpy(metrics, &m_metrics, sizeof(struct hio_lte_metrics));
	k_mutex_unlock(&m_metrics_lock);
	return 0;
}

int hio_lte_get_fsm_state(const char **state)
{
	if (!state) {
		return -EINVAL;
	}
	k_mutex_lock(&m_state_lock, K_FOREVER);
	*state = fsm_state_str(m_state);
	k_mutex_unlock(&m_state_lock);
	return 0;
}

int hio_lte_add_callback(struct hio_lte_cb *cb)
{
	if (!cb || !cb->handler) {
		return -EINVAL;
	}

	struct hio_lte_cb *iter;
	SYS_SLIST_FOR_EACH_CONTAINER(&cb_list, iter, node) {
		if (iter == cb) {
			return -EALREADY;
		}
	}

	sys_slist_append(&cb_list, &cb->node);
	return 0;
}

int hio_lte_remove_callback(struct hio_lte_cb *cb)
{
	if (!cb) {
		return -EINVAL;
	}

	bool removed = sys_slist_find_and_remove(&cb_list, &cb->node);
	return removed ? 0 : -ENOENT;
}

int hio_lte_get_ncellmeas_param(struct hio_lte_ncellmeas_param *param)
{
	return hio_lte_state_get_ncellmeas_param(param);
}

int hio_lte_schedule_ncellmeas(void)
{
	if (g_hio_lte_config.test) {
		LOG_WRN("LTE Test mode enabled");
		return -ENOTSUP;
	}

	if (atomic_test_and_set_bit(&m_flag, FLAG_NCELLMEAS_REQ)) {
		return -EALREADY;
	}

	k_mutex_lock(&m_state_lock, K_FOREVER);
	enum fsm_state current = m_state;
	k_mutex_unlock(&m_state_lock);

	if (current == FSM_STATE_SLEEP) {
		delegate_event(HIO_LTE_FSM_EVENT_READY);
	}

	return 0;
}

static int on_enter_disabled(void)
{
	int ret;

	ret = hio_lte_flow_stop();
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_stop` failed: %d", ret);
	}

	k_event_clear(&m_states_event, ATTACHED_BIT | CONNECTED_BIT);
	m_error_ctx.flow_check_failures = 0;

	start_timer(K_SECONDS(5));

	return 0;
}

static int on_leave_disabled(void)
{
	memset(&m_error_ctx, 0, sizeof(m_error_ctx));
	m_attach_retry_count = 0;

	atomic_clear_bit(&m_flag, FLAG_CSCON);
	atomic_clear_bit(&m_flag, FLAG_CFUN4);
	atomic_clear_bit(&m_flag, FLAG_DTLS_SAVED);
	// atomic_clear_bit(&m_flag, FLAG_NCELLMEAS_REQ);
	atomic_set_bit(&m_flag, FLAG_NCELLMEAS_REQ);

	return 0;
}

static int disabled_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_ENABLE:
		transition_state(FSM_STATE_PREPARE);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	case HIO_LTE_FSM_EVENT_DEREGISTERED:
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int on_enter_error(void)
{
	m_error_ctx.on_timeout_state = FSM_STATE_PREPARE; /* Default timeout state */

	int ret = hio_lte_flow_check();
	if (ret == 0) {
		m_error_ctx.flow_check_failures = 0;
		LOG_INF("Flow check successful, resuming operation in 5 seconds");
		m_error_ctx.on_timeout_state = FSM_STATE_READY;
		start_timer(K_SECONDS(5));
		return 0;
	}

	k_event_clear(&m_states_event, ATTACHED_BIT | CONNECTED_BIT);

	if (ret == -ENOTSOCK) {
		m_error_ctx.flow_check_failures++;

		LOG_INF("failed (attempt %d/%d): %d", m_error_ctx.flow_check_failures,
			ON_ERROR_MAX_FLOW_CHECK_RETRIES, ret);

		if (m_error_ctx.flow_check_failures < ON_ERROR_MAX_FLOW_CHECK_RETRIES) {
			LOG_ERR("Socket error, retrying open in 5 seconds");
			m_error_ctx.on_timeout_state = FSM_STATE_OPEN_SOCKET;
			start_timer(K_SECONDS(5));
			return 0;
		}

		LOG_ERR("Max retries reached, taking fallback action");
		m_error_ctx.flow_check_failures = 0; /* Reset counter */
	}

	ret = hio_lte_flow_stop();
	if (ret) {
		LOG_ERR("Call `hio_lte_flow_stop` failed: %d", ret);
	}

	m_error_ctx.timeout_s += 10;
	if (m_error_ctx.timeout_s > 600) { /* Max 10 minutes */
		m_error_ctx.timeout_s = 600;
	}

	LOG_INF("Waiting %u seconds before next reconnect attempt", m_error_ctx.timeout_s);
	start_timer(K_SECONDS(m_error_ctx.timeout_s));

	return 0;
}

static int error_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		transition_state(m_error_ctx.on_timeout_state);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_prepare(void)
{
	int ret;

	ret = hio_lte_flow_start();
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_start` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_flow_prepare();
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_prepare` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_flow_cfun(1);
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_cfun` failed: %d", ret);
		return ret;
	}

	start_timer(SIMDETECTED_TIMEOUT);

	return 0;
}

static int prepare_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_SIMDETECTED:
		stop_timer();
		int ret = hio_lte_flow_sim_info();
		if (ret < 0) {
			LOG_ERR("Call `hio_lte_flow_sim_info` failed: %d", ret);
			return ret;
		}

		ret = hio_lte_flow_sim_fplmn();
		if (ret) {
			if (ret == -EAGAIN) {
				break;
			} else if (ret == -EOPNOTSUPP) {
				LOG_WRN("FPLMN Erase not supported, continuing");
			} else {
				LOG_ERR("Call `hio_lte_flow_sim_fplmn` failed: %d", ret);
				return ret;
			}
		}
		transition_state(FSM_STATE_ATTACH);
		break;
	case HIO_LTE_FSM_EVENT_RESET_LOOP:
		transition_state(FSM_STATE_RESET_LOOP);
		break;
	case HIO_LTE_FSM_EVENT_TIMEOUT:
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_leave_prepare(void)
{
	stop_timer();
	return 0;
}

static int on_enter_reset_loop(void)
{
	int ret;

	ret = hio_lte_flow_cfun(4);
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_cfun` failed: %d", ret);
		return ret;
	}

	k_sleep(K_SECONDS(5));

	start_timer(MDMEV_RESET_LOOP_DELAY);

	m_attach_retry_count = 0;

	return 0;
}

static int reset_loop_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		transition_state(FSM_STATE_PREPARE);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_leave_reset_loop(void)
{
	/* for: nrf9160
	int ret = hio_lte_flow_cfun(0);
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_cfun` failed: %d", ret);
		return ret;
	}

	k_sleep(K_SECONDS(5));

	ret = hio_lte_flow_cfun(1);
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_cfun` failed: %d", ret);
		return ret;
	}
	*/

	int ret = hio_lte_flow_stop();
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_stop` failed: %d", ret);
	}

	start_timer(K_SECONDS(10));

	return 0;
}

static int on_enter_retry_delay(void)
{
	int ret = hio_lte_flow_cfun(4);
	if (ret < 0) {
		LOG_WRN("Call `hio_lte_flow_cfun` failed: %d", ret);
	}

	k_sleep(K_SECONDS(5));

	struct hio_lte_attach_timeout timeout = get_attach_timeout(m_attach_retry_count);

	LOG_INF("Waiting %lld minutes before attach retry",
		k_ticks_to_ms_floor64(timeout.retry_delay.ticks) / MSEC_PER_SEC / 60);

	start_timer(timeout.retry_delay);

	m_start = k_uptime_get_32();

	return 0;
}

static int retry_delay_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		transition_state(FSM_STATE_PREPARE);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_leave_retry_delay(void)
{
	stop_timer();
	m_attach_retry_count++; /* Increment retry count */
	return 0;
}

static int on_enter_attach(void)
{
	k_mutex_lock(&m_metrics_lock, K_FOREVER);
	m_metrics.attach_count++;
	int ret = hio_rtc_get_ts(&m_metrics.attach_last_ts);
	if (ret) {
		LOG_WRN("Call `hio_rtc_get_ts` failed: %d", ret);
	}
	k_mutex_unlock(&m_metrics_lock);

	m_start = k_uptime_get_32();

	k_event_clear(&m_states_event, ATTACHED_BIT | CONNECTED_BIT);

	struct hio_lte_attach_timeout timeout = get_attach_timeout(m_attach_retry_count);

	LOG_INF("Try to attach with timeout %lld s",
		k_ticks_to_ms_floor64(timeout.attach_timeout.ticks) / MSEC_PER_SEC);

	start_timer(timeout.attach_timeout);

	return 0;
}

static int attach_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_REGISTERED:
		m_attach_retry_count = 0;
		k_mutex_lock(&m_metrics_lock, K_FOREVER);
		m_metrics.attach_last_duration_ms = k_uptime_get_32() - m_start;
		m_metrics.attach_duration_ms += m_metrics.attach_last_duration_ms;
		k_mutex_unlock(&m_metrics_lock);
		k_event_post(&m_states_event, ATTACHED_BIT);
		transition_state(FSM_STATE_OPEN_SOCKET);
		break;
	case HIO_LTE_FSM_EVENT_RESET_LOOP:
		transition_state(FSM_STATE_RESET_LOOP);
		break;
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		k_mutex_lock(&m_metrics_lock, K_FOREVER);
		m_metrics.attach_fail_count++;
		m_metrics.attach_last_duration_ms = k_uptime_get_32() - m_start;
		m_metrics.attach_duration_ms += m_metrics.attach_last_duration_ms;
		k_mutex_unlock(&m_metrics_lock);
		transition_state(FSM_STATE_RETRY_DELAY);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_leave_attach(void)
{
	stop_timer();
	return 0;
}

static int on_enter_open_socket(void)
{
	if (strcmp(m_socket_config.addr, "127.0.0.1") == 0) {
		LOG_WRN("Using loopback address, skipping socket open");

		int ret = hio_lte_flow_coneval();
		if (ret < 0) {
			LOG_WRN("Call `hio_lte_flow_coneval` failed: %d", ret);
		}

		return 0;
	}

	bool dtls_saved = atomic_test_and_clear_bit(&m_flag, FLAG_DTLS_SAVED);

	int ret = hio_lte_flow_open_socket(&m_socket_config, dtls_saved);
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_open_socket` failed: %d", ret);
		return ret;
	}

	delegate_event(HIO_LTE_FSM_EVENT_SOCKET_OPENED);

	return 0;
}

static int open_socket_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_SOCKET_OPENED:
		k_event_post(&m_states_event, CONNECTED_BIT);
		transition_state(FSM_STATE_CONEVAL);
		break;
	case HIO_LTE_FSM_EVENT_DEREGISTERED:
		transition_state(FSM_STATE_ATTACH);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_ready(void)
{
	if (m_send_recv_param) {
		delegate_event(HIO_LTE_FSM_EVENT_SEND);
	}

	start_timer(K_MSEC(500));

	return 0;
}

static int ready_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_SEND:
		if (atomic_test_bit(&m_flag, FLAG_CFUN4)) {
			return 0; /* ignore SEND event */
		}
		stop_timer();
		int ret = hio_lte_flow_check();
		if (ret < 0) {
			return ret;
		}
		transition_state(FSM_STATE_SEND);
		break;
	case HIO_LTE_FSM_EVENT_DEREGISTERED:
		if (atomic_test_bit(&m_flag, FLAG_CFUN4)) {
			return 0; /* ignore DEREGISTERED event */
		}
		transition_state(FSM_STATE_ATTACH);
		break;
	case HIO_LTE_FSM_EVENT_CSCON_0:
		if (atomic_test_bit(&m_flag, FLAG_NCELLMEAS_REQ)) {
			transition_state(FSM_STATE_NCELLMEAS);
		}
		break;
	case HIO_LTE_FSM_EVENT_XMODEMSLEEP:
		transition_state(FSM_STATE_SLEEP);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		if (atomic_test_bit(&m_flag, FLAG_NCELLMEAS_REQ)) {
			return 0; /* ignore TIMEOUT event, wait for CSCON_0 */
		}
		hio_lte_state_get_cereg_param(&m_cereg_param);
		if (m_cereg_param.active_time == -1) {
			int ret = hio_lte_flow_close_socket(m_socket_config.dtls_enabled);
			if (!ret && m_socket_config.dtls_enabled) {
				atomic_set_bit(&m_flag, FLAG_DTLS_SAVED);
			}
			LOG_WRN("PSM is not supported, disabling LTE modem to save power");
			ret = hio_lte_flow_cfun(4);
			if (ret < 0) {
				LOG_ERR("Call `hio_lte_flow_cfun` failed: %d", ret);
				return ret;
			}
			atomic_set_bit(&m_flag, FLAG_CFUN4);
		}
		return 0;
	default:
		break;
	}
	return 0;
}

static int on_leave_ready(void)
{
	stop_timer();
	return 0;
}

static int on_enter_sleep(void)
{
	if (m_send_recv_param) {
		delegate_event(HIO_LTE_FSM_EVENT_SEND);
	}

	return 0;
}

static int sleep_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_SEND:
		__fallthrough;
	case HIO_LTE_FSM_EVENT_READY:
		if (atomic_test_bit(&m_flag, FLAG_CFUN4)) {
			int ret = hio_lte_flow_cfun(1); /* Exit power save mode */
			if (ret < 0) {
				LOG_ERR("Call `hio_lte_flow_cfun` failed: %d", ret);
				return ret;
			}
			atomic_clear_bit(&m_flag, FLAG_CFUN4);
			transition_state(FSM_STATE_ATTACH);
			return 0;
		}
		if (event == HIO_LTE_FSM_EVENT_SEND) {
			transition_state(FSM_STATE_SEND);
		} else {
			transition_state(FSM_STATE_READY);
		}
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_send(void)
{
	int ret;

	if (!m_send_recv_param) {
		delegate_event(HIO_LTE_FSM_EVENT_READY);
		return 0;
	}

	k_mutex_lock(&m_metrics_lock, K_FOREVER);
	m_metrics.uplink_count++;
	m_metrics.uplink_bytes += m_send_recv_param->send_len;
	ret = hio_rtc_get_ts(&m_metrics.uplink_last_ts);
	if (ret) {
		LOG_ERR("Call `hio_rtc_get_ts` failed: %d", ret);
	}
	k_mutex_unlock(&m_metrics_lock);

	ret = hio_lte_flow_send(m_send_recv_param);
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_send` failed: %d", ret);

		k_mutex_lock(&m_metrics_lock, K_FOREVER);
		m_metrics.uplink_errors++;
		k_mutex_unlock(&m_metrics_lock);

		return ret;
	}

	start_timer(SEND_CSCON_1_TIMEOUT);

	if (atomic_test_bit(&m_flag, FLAG_CSCON)) {
		delegate_event(HIO_LTE_FSM_EVENT_SEND);
	}

	return 0;
}

static int send_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_CSCON_0:
		transition_state(FSM_STATE_READY);
		break;
	case HIO_LTE_FSM_EVENT_CSCON_1:
		__fallthrough;
	case HIO_LTE_FSM_EVENT_SEND:
		stop_timer();
		if (m_send_recv_param) {
			LOG_INF("Send event on send state");
			if (m_send_recv_param->recv_buf) {
				transition_state(FSM_STATE_RECEIVE);
			} else {
				if (m_send_recv_param->rai) {
					k_sleep(K_MSEC(500));
				}
				m_send_recv_param = NULL;
				k_event_post(&m_states_event, SEND_RECV_BIT);
				transition_state(FSM_STATE_CONEVAL);
			}
		} else {
			transition_state(FSM_STATE_READY);
		}
		break;
	case HIO_LTE_FSM_EVENT_READY:
		__fallthrough;
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		k_mutex_lock(&m_metrics_lock, K_FOREVER);
		m_metrics.uplink_errors++;
		k_mutex_unlock(&m_metrics_lock);
		transition_state(FSM_STATE_READY);
		break;
	case HIO_LTE_FSM_EVENT_DEREGISTERED:
		transition_state(FSM_STATE_ATTACH);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_leave_send(void)
{
	stop_timer();
	return 0;
}

static int on_enter_receive(void)
{
	int ret;
	LOG_INF("on_enter_receive");

	if (!m_send_recv_param) {
		delegate_event(HIO_LTE_FSM_EVENT_READY);
		return 0;
	}

	k_mutex_lock(&m_metrics_lock, K_FOREVER);
	m_metrics.downlink_count++;
	ret = hio_rtc_get_ts(&m_metrics.downlink_last_ts);
	if (ret) {
		LOG_ERR("Call `hio_rtc_get_ts` failed: %d", ret);
	}
	k_mutex_unlock(&m_metrics_lock);

	ret = hio_lte_flow_recv(m_send_recv_param);
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_recv` failed: %d", ret);

		k_mutex_lock(&m_metrics_lock, K_FOREVER);
		m_metrics.downlink_errors++;
		k_mutex_unlock(&m_metrics_lock);

		return ret;
	}

	k_mutex_lock(&m_metrics_lock, K_FOREVER);
	m_metrics.downlink_bytes += *m_send_recv_param->recv_len;
	k_mutex_unlock(&m_metrics_lock);

	k_sleep(K_MSEC(100));
	delegate_event(HIO_LTE_FSM_EVENT_RECV);

	m_send_recv_param = NULL;
	k_event_post(&m_states_event, SEND_RECV_BIT);

	return 0;
}

static int receive_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_RECV:
		if (!m_send_recv_param) {
			transition_state(FSM_STATE_CONEVAL);
			return 0;
		}
		__fallthrough;
	case HIO_LTE_FSM_EVENT_READY:
		__fallthrough;
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		transition_state(FSM_STATE_READY);
		break;
	case HIO_LTE_FSM_EVENT_DEREGISTERED:
		transition_state(FSM_STATE_ATTACH);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_coneval(void)
{
	int ret = hio_lte_flow_coneval();
	if (ret < 0) {
		LOG_WRN("Call `hio_lte_flow_coneval` failed: %d", ret);
	}

	delegate_event(HIO_LTE_FSM_EVENT_READY);

	return 0;
}

static int coneval_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_READY:
		__fallthrough;
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		transition_state(FSM_STATE_READY);
		break;
	case HIO_LTE_FSM_EVENT_DEREGISTERED:
		transition_state(FSM_STATE_ATTACH);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_ncellmeas(void)
{
	start_timer(NCELLMEAS_TIMEOUT);

	int ret = hio_lte_talk_ncellmeas(5, HIO_LTE_NCELLMEAS_CELL_MAX);
	if (ret < 0) {
		LOG_ERR("Enable NCELLMEAS failed: %d", ret);
		return ret;
	}

	return 0;
}

static int ncellmeas_event_handler(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		transition_state(FSM_STATE_READY);
		break;
	case HIO_LTE_FSM_EVENT_NCELLMEAS:
		atomic_clear_bit(&m_flag, FLAG_NCELLMEAS_REQ);
		hio_lte_notify(HIO_LTE_EVENT_NCELLMEAS_DONE);
		transition_state(FSM_STATE_READY);
		break;
	case HIO_LTE_FSM_EVENT_ERROR:
		transition_state(FSM_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_leave_ncellmeas(void)
{
	stop_timer();

	int ret = hio_lte_flow_cmd("AT%NCELLMEASSTOP");
	if (ret < 0) {
		LOG_WRN("Disable NCELLMEAS failed: %d", ret);
	}

	return 0;
}

/* clang-format off */
static struct fsm_state_desc m_fsm_states[] = {
	{FSM_STATE_DISABLED, on_enter_disabled, on_leave_disabled, disabled_event_handler},
	{FSM_STATE_ERROR, on_enter_error, NULL, error_event_handler},
	{FSM_STATE_PREPARE, on_enter_prepare, on_leave_prepare, prepare_event_handler},
	{FSM_STATE_ATTACH, on_enter_attach, on_leave_attach, attach_event_handler},
	{FSM_STATE_RETRY_DELAY, on_enter_retry_delay, on_leave_retry_delay, retry_delay_event_handler},
	{FSM_STATE_RESET_LOOP, on_enter_reset_loop, on_leave_reset_loop, reset_loop_event_handler},
	{FSM_STATE_OPEN_SOCKET, on_enter_open_socket, NULL, open_socket_event_handler},
	{FSM_STATE_READY, on_enter_ready, on_leave_ready, ready_event_handler},
	{FSM_STATE_SLEEP, on_enter_sleep, NULL, sleep_event_handler},
	{FSM_STATE_SEND, on_enter_send, on_leave_send, send_event_handler},
	{FSM_STATE_RECEIVE, on_enter_receive, NULL, receive_event_handler},
	{FSM_STATE_CONEVAL, on_enter_coneval, NULL, coneval_event_handler},
	{FSM_STATE_NCELLMEAS, on_enter_ncellmeas, on_leave_ncellmeas, ncellmeas_event_handler},
};
/* clang-format on */

static struct fsm_state_desc *get_fsm_state(enum fsm_state state)
{
	for (size_t i = 0; i < ARRAY_SIZE(m_fsm_states); i++) {
		if (m_fsm_states[i].state == state) {
			return &m_fsm_states[i];
		}
	}

	LOG_ERR("Unknown state: %s %d", fsm_state_str(state), state);

	return NULL;
}

static int init(void)
{
	int ret;

	LOG_INF("System initialization");

	sys_slist_init(&cb_list);

	ret = hio_lte_config_init();
	if (ret) {
		LOG_ERR("Call `hio_lte_config_init` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_flow_init(delegate_event);
	if (ret) {
		LOG_ERR("Call `hio_lte_flow_init` failed: %d", ret);
		return ret;
	}

	m_state = FSM_STATE_DISABLED;

	k_work_queue_init(&m_work_q);

	k_work_queue_start(&m_work_q, m_work_q_stack, K_THREAD_STACK_SIZEOF(m_work_q_stack),
			   WORK_Q_PRIORITY, NULL);

	k_work_init_delayable(&m_timeout_work, timeout_work_handler);
	k_work_init(&m_event_dispatch_work, event_dispatch_work_handler);
	ring_buf_init(&m_event_rb, sizeof(m_event_buf), m_event_buf);

	return 0;
}

SYS_INIT(init, APPLICATION, CONFIG_HIO_LTE_INIT_PRIORITY);
