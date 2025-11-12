#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/zbus/zbus.h>
#include <time.h>
#include <string.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

struct time_message {
	uint64_t epoch_seconds;
	uint16_t milliseconds;
	int64_t epoch_ms;
	struct tm utc_time;
};

ZBUS_MSG_SUBSCRIBER_DEFINE(sub_logger);
ZBUS_MSG_SUBSCRIBER_DEFINE(sub_app);

ZBUS_CHAN_DEFINE(chan_time, struct time_message, NULL, NULL,
	ZBUS_OBSERVERS(sub_logger, sub_app),
	ZBUS_MSG_INIT(.epoch_seconds = 0, .milliseconds = 0, .epoch_ms = 0));

#define STACK_SZ 2048
#define PRIO_SNTP 4
#define PRIO_LOGGER 5
#define PRIO_APP 5

static uint16_t frac_to_ms(uint32_t f) {
	return (uint16_t)(((uint64_t)f * 1000U) >> 32);
}

static void utc_from_epoch(uint64_t sec, struct tm *out) {
	time_t s = (time_t)MIN(sec, (uint64_t)SYS_TIME_T_MAX);
	if (!gmtime_r(&s, out)) memset(out, 0, sizeof(*out));
}

static void thread_sntp(void *, void *, void *) {
	uint16_t port = CONFIG_SNTP_SERVER_PORT;
	uint32_t interval = CONFIG_SNTP_UPDATE_INTERVAL_SEC * 1000U;
	k_thread_name_set(k_current_get(), "sntp");
	k_sleep(K_SECONDS(1));

	while (1) {
		struct sntp_time t = {0};
		int r = sntp_simple(CONFIG_SNTP_SERVER_HOSTNAME, port, &t);
		if (!r) {
			struct time_message msg = {
				.epoch_seconds = t.seconds,
				.milliseconds = frac_to_ms(t.fraction)
			};
			msg.epoch_ms = (int64_t)msg.epoch_seconds * 1000 + msg.milliseconds;
			utc_from_epoch(msg.epoch_seconds, &msg.utc_time);
			LOG_INF("SNTP: %s:%u -> %lld ms", CONFIG_SNTP_SERVER_HOSTNAME, port, msg.epoch_ms);
			if (zbus_chan_pub(&chan_time, &msg, K_SECONDS(1)) != 0)
				LOG_ERR("Falha ao publicar tempo");
		} else {
			LOG_WRN("SNTP falhou (%d)", r);
		}
		k_msleep(interval);
	}
}

static void thread_logger(void *, void *, void *) {
	const struct zbus_observer *s = &sub_logger;
	const struct zbus_channel *c;
	struct time_message m;
	k_thread_name_set(k_current_get(), "logger");
	while (zbus_sub_wait_msg(s, &c, &m, K_FOREVER) == 0) {
		if (c != &chan_time) continue;
		LOG_INF("[Logger] %04d-%02d-%02d %02d:%02d:%02d.%03u UTC (epoch_ms=%lld)",
			m.utc_time.tm_year + 1900, m.utc_time.tm_mon + 1, m.utc_time.tm_mday,
			m.utc_time.tm_hour, m.utc_time.tm_min, m.utc_time.tm_sec,
			m.milliseconds, m.epoch_ms);
	}
}

static void thread_app(void *, void *, void *) {
	const struct zbus_observer *s = &sub_app;
	const struct zbus_channel *c;
	struct time_message m;
	int64_t last = -1;
	k_thread_name_set(k_current_get(), "app");
	while (zbus_sub_wait_msg(s, &c, &m, K_FOREVER) == 0) {
		if (c != &chan_time) continue;
		if (last >= 0) {
			LOG_INF("[App] Δt = %lld ms", m.epoch_ms - last);
		} else {
			LOG_INF("[App] Primeira sincronizacao");
		}
		last = m.epoch_ms;
	}
}

K_THREAD_DEFINE(t_sntp, STACK_SZ, thread_sntp, NULL, NULL, NULL, PRIO_SNTP, 0, 0);
K_THREAD_DEFINE(t_logger, STACK_SZ, thread_logger, NULL, NULL, NULL, PRIO_LOGGER, 0, 0);
K_THREAD_DEFINE(t_app, STACK_SZ, thread_app, NULL, NULL, NULL, PRIO_APP, 0, 0);

int main(void) {
	LOG_INF("Sincronizacao SNTP iniciada");
	for (;;) k_sleep(K_FOREVER);
}
