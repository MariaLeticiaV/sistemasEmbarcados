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

ZBUS_MSG_SUBSCRIBER_DEFINE(logger_sub);
ZBUS_MSG_SUBSCRIBER_DEFINE(app_sub);

ZBUS_CHAN_DEFINE(time_channel, struct time_message, NULL, NULL,
		 ZBUS_OBSERVERS(logger_sub, app_sub),
		 ZBUS_MSG_INIT(.epoch_seconds = 0, .milliseconds = 0,
			       .epoch_ms = 0));

#define SNTP_STACK_SIZE 2048
#define LISTENER_STACK_SIZE 2048
#define APPLICATION_STACK_SIZE 2048

#define SNTP_THREAD_PRIORITY 4
#define LOGGER_THREAD_PRIORITY 5
#define APPLICATION_THREAD_PRIORITY 5

static uint16_t fractional_to_ms(uint32_t fraction)
{
	return (uint16_t)(((uint64_t)fraction * 1000U) >> 32);
}

static void fill_utc_time(uint64_t epoch_seconds, struct tm *out_tm)
{
	const time_t seconds32 =
		(time_t)MIN(epoch_seconds, (uint64_t)SYS_TIME_T_MAX);

	if (gmtime_r(&seconds32, out_tm) == NULL) {
		memset(out_tm, 0, sizeof(*out_tm));
	}
}

static void sntp_client_thread(void *, void *, void *)
{
	const uint16_t port = CONFIG_SNTP_SERVER_PORT;
	const uint32_t interval_ms =
		(uint32_t)CONFIG_SNTP_UPDATE_INTERVAL_SEC * 1000U;

	k_thread_name_set(k_current_get(), "sntp-client");

	k_sleep(K_SECONDS(1));

	while (true) {
		struct sntp_time sntp_time = { 0 };
		int ret = sntp_simple(CONFIG_SNTP_SERVER_HOSTNAME, port,
				      &sntp_time);

		if (ret == 0) {
			struct time_message msg = {
				.epoch_seconds = sntp_time.seconds,
				.milliseconds =
					fractional_to_ms(sntp_time.fraction),
			};

			msg.epoch_ms =
				(int64_t)msg.epoch_seconds * 1000LL +
				msg.milliseconds;
			fill_utc_time(msg.epoch_seconds, &msg.utc_time);

			LOG_INF("SNTP sincronizado: %s:%u -> %lld ms",
				CONFIG_SNTP_SERVER_HOSTNAME, port,
				msg.epoch_ms);

			ret = zbus_chan_pub(&time_channel, &msg, K_SECONDS(1));
			if (ret != 0) {
				LOG_ERR("Falha ao publicar tempo (%d)", ret);
			}
		} else {
			LOG_WRN("SNTP falhou (%d), mantendo ultimo horario",
				ret);
		}

		k_msleep(interval_ms);
	}
}

static void logger_thread(void *, void *, void *)
{
	const struct zbus_observer *sub = &logger_sub;
	const struct zbus_channel *chan = NULL;
	struct time_message msg;

	k_thread_name_set(k_current_get(), "logger");

	while (zbus_sub_wait_msg(sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan != &time_channel) {
			continue;
		}

		LOG_INF("[Logger] %04d-%02d-%02d %02d:%02d:%02d.%03u UTC "
			"(epoch_ms=%lld)",
			msg.utc_time.tm_year + 1900, msg.utc_time.tm_mon + 1,
			msg.utc_time.tm_mday, msg.utc_time.tm_hour,
			msg.utc_time.tm_min, msg.utc_time.tm_sec,
			msg.milliseconds, msg.epoch_ms);
	}
}

static void application_thread(void *, void *, void *)
{
	const struct zbus_observer *sub = &app_sub;
	const struct zbus_channel *chan = NULL;
	struct time_message msg;
	int64_t last_epoch_ms = -1;

	k_thread_name_set(k_current_get(), "app");

	while (zbus_sub_wait_msg(sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan != &time_channel) {
			continue;
		}

		if (last_epoch_ms >= 0) {
			const int64_t delta_ms = msg.epoch_ms - last_epoch_ms;

			LOG_INF("[App] Tempo desde ultima sincronizacao: "
				"%lld ms", delta_ms);
		} else {
			LOG_INF("[App] Primeira sincronizacao recebida");
		}

		last_epoch_ms = msg.epoch_ms;
	}
}

K_THREAD_DEFINE(sntp_thread_id, SNTP_STACK_SIZE, sntp_client_thread, NULL,
		NULL, NULL, SNTP_THREAD_PRIORITY, 0, 0);
K_THREAD_DEFINE(logger_thread_id, LISTENER_STACK_SIZE, logger_thread, NULL,
		NULL, NULL, LOGGER_THREAD_PRIORITY, 0, 0);
K_THREAD_DEFINE(app_thread_id, APPLICATION_STACK_SIZE, application_thread,
		NULL, NULL, NULL, APPLICATION_THREAD_PRIORITY, 0, 0);

int main(void)
{
	LOG_INF("Sistema de sincronizacao SNTP iniciado");

	while (true) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
