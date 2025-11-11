#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

static void timer_cb(struct k_timer *t)
{
	static uint32_t count;
	ARG_UNUSED(t);
	LOG_INF("Hello World!");
	LOG_DBG("Tick #%u", count);
	if ((count++ % 10U) == 0U) {
		LOG_ERR("Erro a cada 10 ticks");
	}
}

K_TIMER_DEFINE(timer, timer_cb, NULL);

int main(void)
{
	LOG_INF("Iniciando timer: %d ms", CONFIG_HELLO_TIMER_PERIOD_MS);
	k_timer_start(&timer, K_NO_WAIT, K_MSEC(CONFIG_HELLO_TIMER_PERIOD_MS));
	for (;;) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
