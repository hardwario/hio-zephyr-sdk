#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_state.h"

/* HIO includes */
#include <hio/hio_rtc.h>
#include <hio/hio_lte.h>

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_lte, CONFIG_HIO_LTE_LOG_LEVEL);

#define WAKEUP_TIMEOUT         K_SECONDS(5)
#define SIMDETECTED_TIMEOUT    K_SECONDS(10)
#define MDMEV_RESET_LOOP_DELAY K_MINUTES(32)
#define SEND_CSCON_1_TIMEOUT   K_SECONDS(30)
#define CONEVAL_TIMEOUT        K_SECONDS(30)

enum hio_lte_state {
	HIO_LTE_STATE_DISABLED = 0,
	HIO_LTE_STATE_ERROR,
	HIO_LTE_STATE_PREPARE,
	HIO_LTE_STATE_ATTACH,
	HIO_LTE_STATE_RETRY_DELAY,
	HIO_LTE_STATE_RESET_LOOP,
	HIO_LTE_STATE_OPEN_SOCKET,
	HIO_LTE_STATE_READY,
	HIO_LTE_STATE_SLEEP,
	HIO_LTE_STATE_WAKEUP,
	HIO_LTE_STATE_SEND,
	HIO_LTE_STATE_RECEIVE,
	HIO_LTE_STATE_CONEVAL,
};

struct hio_lte_fsm_state {
	enum hio_lte_state state;
	int (*on_enter)(void);
	int (*on_leave)(void);
	int (*event_handler)(enum hio_lte_event event);
};

struct attach_timeout {
	k_timeout_t retry_delay;
	k_timeout_t attach_timeout;
};

int m_attach_retry_count = 0;
enum hio_lte_state m_state;
struct k_work_delayable m_timeout_work;
struct k_work m_event_dispatch_work;
uint8_t m_event_buf[8];
bool m_cscon = false;
struct ring_buf m_event_rb;
K_MUTEX_DEFINE(m_event_rb_lock);

static K_EVENT_DEFINE(m_states_event);
#define CONNECTED_BIT BIT(0)
#define SEND_RECV_BIT BIT(1)

K_MUTEX_DEFINE(m_send_recv_lock);
struct hio_lte_send_recv_param *m_send_recv_param = NULL;

struct hio_lte_metrics m_metrics;
K_MUTEX_DEFINE(m_metrics_lock);

static struct hio_lte_fsm_state *get_fsm_state(enum hio_lte_state state);

struct attach_timeout get_attach_timeout(int count)
{
	switch (count) {
	case 0:
		return (struct attach_timeout){K_NO_WAIT, K_MINUTES(1)};
	case 1:
		return (struct attach_timeout){K_NO_WAIT, K_MINUTES(5)};
	case 2:
		return (struct attach_timeout){K_MINUTES(5), K_MINUTES(45)};
	case 3:
		return (struct attach_timeout){K_HOURS(1), K_MINUTES(5)};
	case 4:
		return (struct attach_timeout){K_MINUTES(5), K_MINUTES(45)};
	case 5:
		return (struct attach_timeout){K_HOURS(6), K_MINUTES(5)};
	case 6:
		return (struct attach_timeout){K_MINUTES(5), K_MINUTES(45)};
	case 7:
		return (struct attach_timeout){K_HOURS(24), K_MINUTES(5)};
	case 8:
		return (struct attach_timeout){K_MINUTES(5), K_MINUTES(45)};
	default:
		if (count % 2 != 0) { /*  9, 11 ... */
			return (struct attach_timeout){K_HOURS(168), K_MINUTES(5)};
		} else { /* 10, 12 ... */
			return (struct attach_timeout){K_MINUTES(5), K_MINUTES(45)};
		}
	}
}

const char *hio_lte_state_str(enum hio_lte_state state)
{
	switch (state) {
	case HIO_LTE_STATE_DISABLED:
		return "DISABLED";
	case HIO_LTE_STATE_ERROR:
		return "ERROR";
	case HIO_LTE_STATE_PREPARE:
		return "PREPARE";
	case HIO_LTE_STATE_ATTACH:
		return "ATTACH";
	case HIO_LTE_STATE_RETRY_DELAY:
		return "RETRY_DELAY";
	case HIO_LTE_STATE_RESET_LOOP:
		return "RESET_LOOP";
	case HIO_LTE_STATE_OPEN_SOCKET:
		return "OPEN_SOCKET";
	case HIO_LTE_STATE_READY:
		return "READY";
	case HIO_LTE_STATE_SLEEP:
		return "SLEEP";
	case HIO_LTE_STATE_WAKEUP:
		return "WAKEUP";
	case HIO_LTE_STATE_SEND:
		return "SEND";
	case HIO_LTE_STATE_RECEIVE:
		return "RECEIVE";
	case HIO_LTE_STATE_CONEVAL:
		return "CONEVAL";
	}
	return "UNKNOWN";
}

const char *hio_lte_event_str(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_ERROR:
		return "ERROR";
	case HIO_LTE_EVENT_TIMEOUT:
		return "TIMEOUT";
	case HIO_LTE_EVENT_ENABLE:
		return "ENABLE";
	case HIO_LTE_EVENT_READY:
		return "READY";
	case HIO_LTE_EVENT_SIMDETECTED:
		return "SIMDETECTED";
	case HIO_LTE_EVENT_REGISTERED:
		return "REGISTERED";
	case HIO_LTE_EVENT_DEREGISTERED:
		return "DEREGISTERED";
	case HIO_LTE_EVENT_RESET_LOOP:
		return "RESET_LOOP";
	case HIO_LTE_EVENT_SOCKET_OPENED:
		return "SOCKET_OPENED";
	case HIO_LTE_EVENT_XMODEMSLEEP:
		return "XMODEMSLEEP";
	case HIO_LTE_EVENT_CSCON_0:
		return "CSCON_0";
	case HIO_LTE_EVENT_CSCON_1:
		return "CSCON_1";
	case HIO_LTE_EVENT_XTIME:
		return "XTIME";
	case HIO_LTE_EVENT_SEND:
		return "SEND";
	case HIO_LTE_EVENT_RECV:
		return "RECV";
	case HIO_LTE_EVENT_XGPS_ENABLE:
		return "XGPS_ENABLE";
	case HIO_LTE_EVENT_XGPS_DISABLE:
		return "XGPS_DISABLE";
	case HIO_LTE_EVENT_XGPS:
		return "XGPS";
	}
	return "UNKNOWN";
}

const char *hio_lte_coneval_result_str(int result)
{
	switch (result) {
	case 0:
		return "Connection pre-evaluation successful";
	case 1:
		return "Evaluation failed, no cell available";
	case 2:
		return "Evaluation failed, UICC not available";
	case 3:
		return "Evaluation failed, only barred cells available";
	case 4:
		return "Evaluation failed, busy";
	case 5:
		return "Evaluation failed, aborted because of higher priority operation";
	case 6:
		return "Evaluation failed, not registered";
	case 7:
		return "Evaluation failed, unspecified";
	default:
		return "Evaluation failed, unknown result";
	}
}

const char *hio_lte_get_state(void)
{
	return hio_lte_state_str(m_state);
}

static void start_timer(k_timeout_t timeout)
{
	k_work_schedule(&m_timeout_work, timeout);
}

static void stop_timer(void)
{
	k_work_cancel_delayable(&m_timeout_work);
}

static void delegate_event(enum hio_lte_event event)
{
	k_mutex_lock(&m_event_rb_lock, K_FOREVER);
	ring_buf_put(&m_event_rb, (uint8_t *)&event, 1);
	k_mutex_unlock(&m_event_rb_lock);
	k_work_submit(&m_event_dispatch_work);
}

static void event_handler(enum hio_lte_event event)
{
	LOG_INF("event: %s, state: %s", hio_lte_event_str(event), hio_lte_state_str(m_state));

	struct hio_lte_fsm_state *fsm_state = get_fsm_state(m_state);
	if (fsm_state && fsm_state->event_handler) {
		int ret = fsm_state->event_handler(event);
		if (ret < 0) {
			LOG_WRN("failed to handle event, error: %i", ret);
			if (event != HIO_LTE_EVENT_ERROR) {
				delegate_event(HIO_LTE_EVENT_ERROR);
			}
		}
	}
}

static void enter_state(enum hio_lte_state state)
{
	LOG_INF("leaving state: %s", hio_lte_state_str(m_state));
	struct hio_lte_fsm_state *fsm_state = get_fsm_state(m_state);
	if (fsm_state && fsm_state->on_leave) {
		int ret = fsm_state->on_leave();
		if (ret < 0) {
			LOG_WRN("failed to leave state, error: %i", ret);
			if (state != HIO_LTE_STATE_ERROR) {
				delegate_event(HIO_LTE_EVENT_ERROR);
			}
			return;
		}
	}

	m_state = state;
	LOG_DBG("entering to state: %s", hio_lte_state_str(state));
	fsm_state = get_fsm_state(state);
	if (fsm_state && fsm_state->on_enter) {
		int ret = fsm_state->on_enter();
		if (ret < 0) {
			LOG_WRN("failed to enter state error: %i", ret);
			if (state != HIO_LTE_STATE_ERROR) {
				delegate_event(HIO_LTE_EVENT_ERROR);
			}
			return;
		}
	}
}

static void timeout_work_handler(struct k_work *item)
{
	delegate_event(HIO_LTE_EVENT_TIMEOUT);
}

static void event_dispatch_work_handler(struct k_work *item)
{
	uint8_t events[sizeof(m_event_buf)];
	uint8_t events_cnt;

	k_mutex_lock(&m_event_rb_lock, K_FOREVER);
	events_cnt = (uint8_t)ring_buf_get(&m_event_rb, events, ARRAY_SIZE(events));
	k_mutex_unlock(&m_event_rb_lock);

	for (uint8_t i = 0; i < events_cnt; i++) {
		event_handler((enum hio_lte_event)events[i]);
	}
}

int hio_lte_enable(void)
{
	if (g_hio_lte_config.test) {
		LOG_WRN("LTE Test mode enabled");
		return -ENOTSUP;
	}
	delegate_event(HIO_LTE_EVENT_ENABLE);
	return 0;
}

int hio_lte_is_attached(bool *attached)
{
	*attached = k_event_test(&m_states_event, CONNECTED_BIT) ? true : false;
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
	LOG_INF("GET LOCK");

	k_timepoint_t end = sys_timepoint_calc(param->timeout);

	k_mutex_lock(&m_send_recv_lock, sys_timepoint_timeout(end));

	LOG_INF("LOCKED");

	m_send_recv_param = (struct hio_lte_send_recv_param *)param;

	k_event_clear(&m_states_event, SEND_RECV_BIT);

	delegate_event(HIO_LTE_EVENT_SEND);

	LOG_INF("WAITING for send/recv");

	k_event_wait(&m_states_event, SEND_RECV_BIT, false, sys_timepoint_timeout(end));

	if (sys_timepoint_expired(end)) {
		k_mutex_unlock(&m_send_recv_lock);
		delegate_event(HIO_LTE_EVENT_TIMEOUT);
		return -ETIMEDOUT;
	}

	k_mutex_unlock(&m_send_recv_lock);

	LOG_INF("UNLOCK");

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

static int on_enter_disabled(void)
{
	int ret;
	ret = hio_lte_flow_stop();
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int disabled_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_ENABLE:
		enter_state(HIO_LTE_STATE_PREPARE);
		break;
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int on_enter_error(void)
{
	int ret;

	k_event_clear(&m_states_event, CONNECTED_BIT);

	ret = hio_lte_flow_stop();
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_stop` failed: %d", ret);
	}

	start_timer(K_SECONDS(10));

	return 0;
}

static int error_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_TIMEOUT:
		enter_state(HIO_LTE_STATE_PREPARE);
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

static int prepare_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_SIMDETECTED:
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
			}

			LOG_ERR("Call `hio_lte_flow_sim_fplmn` failed: %d", ret);
			return ret;
		}
		enter_state(HIO_LTE_STATE_ATTACH);
		break;
	case HIO_LTE_EVENT_RESET_LOOP:
		enter_state(HIO_LTE_STATE_RESET_LOOP);
		break;
	case HIO_LTE_EVENT_TIMEOUT:
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
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

	return 0;
}

static int reset_loop_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_TIMEOUT:
		enter_state(HIO_LTE_STATE_ATTACH);
		break;
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_leave_reset_loop(void)
{
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

	return 0;
}

static int on_enter_retry_delay(void)
{
	int ret = hio_lte_flow_cfun(4);
	if (ret < 0) {
		LOG_WRN("Call `hio_lte_flow_cfun` failed: %d", ret);
	}

	k_sleep(K_SECONDS(5));

	struct attach_timeout timeout = get_attach_timeout(m_attach_retry_count++);

	LOG_INF("Waiting %lld minutes before attach retry",
		k_ticks_to_ms_floor64(timeout.retry_delay.ticks) / MSEC_PER_SEC / 60);

	start_timer(timeout.retry_delay);
	return 0;
}

static int retry_delay_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_TIMEOUT:
		enter_state(HIO_LTE_STATE_ATTACH);
		break;
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_attach(void)
{
	k_event_clear(&m_states_event, CONNECTED_BIT);

	struct attach_timeout timeout = get_attach_timeout(m_attach_retry_count);

	LOG_INF("Try to attach with timeout %lld s",
		k_ticks_to_ms_floor64(timeout.attach_timeout.ticks) / MSEC_PER_SEC);

	start_timer(timeout.attach_timeout);

	return 0;
}

static int attach_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_REGISTERED:
		m_attach_retry_count = 0;
		enter_state(HIO_LTE_STATE_OPEN_SOCKET);
		break;
	case HIO_LTE_EVENT_CSCON_1:
		m_cscon = true;
		break;
	case HIO_LTE_EVENT_CSCON_0:
		m_cscon = false;
		break;
	case HIO_LTE_EVENT_RESET_LOOP:
		m_attach_retry_count = 0;
		enter_state(HIO_LTE_STATE_RESET_LOOP);
		break;
	case HIO_LTE_EVENT_TIMEOUT:
		enter_state(HIO_LTE_STATE_RETRY_DELAY);
		break;
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
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
	int ret = hio_lte_flow_open_socket();
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_flow_open_socket` failed: %d", ret);
		return ret;
	}

	delegate_event(HIO_LTE_EVENT_SOCKET_OPENED);

	return 0;
}

static int open_socket_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_SOCKET_OPENED:
		k_event_set(&m_states_event, CONNECTED_BIT);
		enter_state(HIO_LTE_STATE_CONEVAL);
		break;
	case HIO_LTE_EVENT_CSCON_0:
		m_cscon = false;
		break;
	case HIO_LTE_EVENT_CSCON_1:
		m_cscon = true;
		break;
	case HIO_LTE_EVENT_DEREGISTERED:
		enter_state(HIO_LTE_STATE_ATTACH);
		break;
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_ready(void)
{
	if (m_send_recv_param) {
		delegate_event(HIO_LTE_EVENT_SEND);
	}

	return 0;
}

static int ready_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_SEND:
		LOG_INF("hio_lte_flow_check!");
		int ret = hio_lte_flow_check();
		if (ret < 0) {
			LOG_ERR("Call `hio_lte_flow_check` failed: %d", ret);
			if (ret == -ENOTCONN) {
				delegate_event(HIO_LTE_EVENT_DEREGISTERED);
				return 0;
			}
			return ret;
		}
		enter_state(HIO_LTE_STATE_SEND);
		break;
	case HIO_LTE_EVENT_DEREGISTERED:
		enter_state(HIO_LTE_STATE_ATTACH);
		break;
	case HIO_LTE_EVENT_CSCON_0:
		m_cscon = false;
		break;
	case HIO_LTE_EVENT_CSCON_1:
		m_cscon = true;
		break;
	case HIO_LTE_EVENT_XMODEMSLEEP:
		enter_state(HIO_LTE_STATE_SLEEP);
		break;
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_sleep(void)
{
	if (m_send_recv_param) {
		delegate_event(HIO_LTE_EVENT_SEND);
	}

	return 0;
}

static int sleep_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_SEND:
	case HIO_LTE_EVENT_XGPS_ENABLE:
		enter_state(HIO_LTE_STATE_WAKEUP);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_wakeup(void)
{
	start_timer(WAKEUP_TIMEOUT);

	return 0;
}

static int wakeup_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_READY:
		enter_state(HIO_LTE_STATE_READY);
		break;
	case HIO_LTE_EVENT_TIMEOUT:
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static int on_leave_wakeup(void)
{
	stop_timer();
	return 0;
}

static int on_enter_send(void)
{
	int ret;

	if (!m_send_recv_param) {
		delegate_event(HIO_LTE_EVENT_READY);
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

	if (m_cscon) {
		delegate_event(HIO_LTE_EVENT_SEND);
	}

	return 0;
}

static int send_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_CSCON_0:
		m_cscon = false;
		enter_state(HIO_LTE_STATE_READY);
		break;
	case HIO_LTE_EVENT_CSCON_1:
		m_cscon = true;
	case HIO_LTE_EVENT_SEND:
		stop_timer();
		LOG_INF("Send event on send state");
		if (m_send_recv_param) {
			if (m_send_recv_param->recv_buf) {
				enter_state(HIO_LTE_STATE_RECEIVE);
			} else {
				m_send_recv_param = NULL;
				k_event_set(&m_states_event, SEND_RECV_BIT);
				enter_state(HIO_LTE_STATE_CONEVAL);
			}
		} else {
			enter_state(HIO_LTE_STATE_READY);
		}
		break;
	case HIO_LTE_EVENT_READY:
	case HIO_LTE_EVENT_TIMEOUT:
		k_mutex_lock(&m_metrics_lock, K_FOREVER);
		m_metrics.uplink_errors++;
		k_mutex_unlock(&m_metrics_lock);
		enter_state(HIO_LTE_STATE_READY);
		break;
	case HIO_LTE_EVENT_DEREGISTERED:
		enter_state(HIO_LTE_STATE_ATTACH);
		break;
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
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
		delegate_event(HIO_LTE_EVENT_READY);
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

	if (!m_send_recv_param->rai) {
		delegate_event(HIO_LTE_EVENT_RECV);
	}

	m_send_recv_param = NULL;
	k_event_set(&m_states_event, SEND_RECV_BIT);

	return 0;
}

static int receive_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_RECV:
	case HIO_LTE_EVENT_READY:
	case HIO_LTE_EVENT_TIMEOUT:
		enter_state(HIO_LTE_STATE_READY);
		break;
	case HIO_LTE_EVENT_CSCON_0:
		m_cscon = false;
		enter_state(HIO_LTE_STATE_CONEVAL);
		break;
	case HIO_LTE_EVENT_DEREGISTERED:
		enter_state(HIO_LTE_STATE_ATTACH);
		break;
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
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
		LOG_ERR("Call `hio_lte_flow_coneval` failed: %d", ret);
		return ret;
	}

	delegate_event(HIO_LTE_EVENT_READY);

	return 0;
}

static int coneval_event_handler(enum hio_lte_event event)
{
	switch (event) {
	case HIO_LTE_EVENT_READY:
	case HIO_LTE_EVENT_TIMEOUT:
		enter_state(HIO_LTE_STATE_READY);
		break;
	case HIO_LTE_EVENT_CSCON_0:
		m_cscon = false;
		break;
	case HIO_LTE_EVENT_CSCON_1:
		m_cscon = true;
		break;
	case HIO_LTE_EVENT_DEREGISTERED:
		enter_state(HIO_LTE_STATE_ATTACH);
		break;
	case HIO_LTE_EVENT_ERROR:
		enter_state(HIO_LTE_STATE_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

/* clang-format off */
static struct hio_lte_fsm_state m_fsm_states[] = {
	{HIO_LTE_STATE_DISABLED, on_enter_disabled, NULL, disabled_event_handler},
	{HIO_LTE_STATE_ERROR, on_enter_error, NULL, error_event_handler},
	{HIO_LTE_STATE_PREPARE, on_enter_prepare, on_leave_prepare, prepare_event_handler},
	{HIO_LTE_STATE_ATTACH, on_enter_attach, on_leave_attach, attach_event_handler},
	{HIO_LTE_STATE_RETRY_DELAY, on_enter_retry_delay, NULL, retry_delay_event_handler},
	{HIO_LTE_STATE_RESET_LOOP, on_enter_reset_loop, on_leave_reset_loop, reset_loop_event_handler},
	{HIO_LTE_STATE_OPEN_SOCKET, on_enter_open_socket, NULL, open_socket_event_handler},
	{HIO_LTE_STATE_READY, on_enter_ready, NULL, ready_event_handler},
	{HIO_LTE_STATE_SLEEP, on_enter_sleep, NULL, sleep_event_handler},
	{HIO_LTE_STATE_WAKEUP, on_enter_wakeup, on_leave_wakeup, wakeup_event_handler},
	{HIO_LTE_STATE_SEND, on_enter_send, on_leave_send, send_event_handler},
	{HIO_LTE_STATE_RECEIVE, on_enter_receive, NULL, receive_event_handler},
	{HIO_LTE_STATE_CONEVAL, on_enter_coneval, NULL, coneval_event_handler},
};
/* clang-format on */

struct hio_lte_fsm_state *get_fsm_state(enum hio_lte_state state)
{
	for (size_t i = 0; i < ARRAY_SIZE(m_fsm_states); i++) {
		if (m_fsm_states[i].state == state) {
			return &m_fsm_states[i];
		}
	}

	LOG_ERR("Unknown state: %s %d", hio_lte_state_str(state), state);

	return NULL;
}

static int init(void)
{
	int ret;

	LOG_INF("System initialization");

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

	m_state = HIO_LTE_STATE_DISABLED;

	k_work_init_delayable(&m_timeout_work, timeout_work_handler);
	k_work_init(&m_event_dispatch_work, event_dispatch_work_handler);
	ring_buf_init(&m_event_rb, sizeof(m_event_buf), m_event_buf);

	return 0;
}

SYS_INIT(init, APPLICATION, CONFIG_HIO_LTE_INIT_PRIORITY);
