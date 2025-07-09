#ifndef SUBSYS_HIO_LTE_MODEM_TALK_H_
#define SUBSYS_HIO_LTE_MODEM_TALK_H_

/* NRF includes */
#include <nrf_modem_at.h>

/* Standard includes */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*hio_lte_talk_cb)(const char *line, void *user_data);

int hio_lte_talk_(const char *s);
int hio_lte_talk_at_cclk_q(char *buf, size_t size);
int hio_lte_talk_at_ceppi(int p1);
int hio_lte_talk_at_cereg(int p1);
int hio_lte_talk_at_cfun(int p1);
int hio_lte_talk_at_cgauth(int p1, int *p2, const char *p3, const char *p4);
int hio_lte_talk_at_cgdcont(int p1, const char *p2, const char *p3);
int hio_lte_talk_at_cgdcont_q(char *buf, size_t size);
int hio_lte_talk_at_cgerep(int p1);
int hio_lte_talk_at_cgsn(char *buf, size_t size);
int hio_lte_talk_at_cimi(char *buf, size_t size);
int hio_lte_talk_at_iccid(char *buf, size_t size);
int hio_lte_talk_at_cmee(int p1);
int hio_lte_talk_at_cnec(int p1);
int hio_lte_talk_at_coneval(char *buf, size_t size);
int hio_lte_talk_at_cops_q(char *buf, size_t size);
int hio_lte_talk_at_cops(int p1, int *p2, const char *p3);
int hio_lte_talk_at_cpsms(int *p1, const char *p2, const char *p3);
int hio_lte_talk_at_cscon(int p1);
int hio_lte_talk_at_hwversion(char *buf, size_t size);
int hio_lte_talk_at_mdmev(int p1);
int hio_lte_talk_at_rai(int p1);
int hio_lte_talk_at_rel14feat(int p1, int p2, int p3, int p4, int p5);
int hio_lte_talk_at_shortswver(char *buf, size_t size);
int hio_lte_talk_at_xbandlock(int p1, const char *p2);
int hio_lte_talk_at_xdataprfl(int p1);
int hio_lte_talk_at_xmodemsleep(int p1, int *p2, int *p3);
int hio_lte_talk_at_xnettime(int p1, int *p2);
int hio_lte_talk_at_xpofwarn(int p1, int p2);
int hio_lte_talk_at_xsim(int p1);
int hio_lte_talk_at_xsocket(int p1, int *p2, int *p3, char *buf, size_t size);
int hio_lte_talk_at_xsocketopt(int p1, int p2, int *p3);
int hio_lte_talk_at_xsystemmode(int p1, int p2, int p3, int p4);
int hio_lte_talk_at_xtemp(int p1);
int hio_lte_talk_at_xtemphighlvl(int p1);
int hio_lte_talk_at_xtime(int p1);
int hio_lte_talk_at_xversion(char *buf, size_t size);
int hio_lte_talk_at_xmodemtrace(int lvl);
int hio_lte_talk_at(void);
int hio_lte_talk_crsm_176(char *buf, size_t size);
int hio_lte_talk_crsm_214(void);
int hio_lte_talk_at_cmd(const char *s);
int hio_lte_talk_at_cmd_with_resp(const char *s, char *buf, size_t size);
int hio_lte_talk_at_cmd_with_resp_prefix(const char *s, char *buf, size_t size, const char *pfx);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_TALK_H_ */
