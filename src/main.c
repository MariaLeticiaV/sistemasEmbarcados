#include <errno.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

DEFINE_FFF_GLOBALS;

#define LED_GPIO_NODE DT_ALIAS(led0)
#define LED_PWM_NODE DT_ALIAS(pwm_led0)
#define BUTTON_NODE DT_ALIAS(sw0)

BUILD_ASSERT(DT_NODE_HAS_STATUS(LED_GPIO_NODE, okay), "Alias led0 nao definido ou desabilitado");
BUILD_ASSERT(DT_NODE_HAS_STATUS(LED_PWM_NODE, okay), "Alias pwm_led0 nao definido ou desabilitado");
BUILD_ASSERT(DT_NODE_HAS_STATUS(BUTTON_NODE, okay), "Alias sw0 nao definido ou desabilitado");

static const struct gpio_dt_spec led_gpio = GPIO_DT_SPEC_GET(LED_GPIO_NODE, gpios);
static const struct pwm_dt_spec led_pwm = PWM_DT_SPEC_GET(LED_PWM_NODE);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);

enum led_mode {
	LED_MODE_GPIO = 0,
	LED_MODE_PWM,
	LED_MODE_COUNT
};

static const uint32_t pwm_step_us = MAX(1U, (CONFIG_LED_PWM_PERIOD_US * CONFIG_LED_PWM_STEP_PCT) / 100U);

static inline void pwm_apply(uint32_t pulse_us)
{
	int rc = pwm_set_dt(&led_pwm, CONFIG_LED_PWM_PERIOD_US, pulse_us);
	if (rc < 0) {
		LOG_ERR("Falha ao ajustar PWM: %d", rc);
	}
}

static bool read_button_pressed(void)
{
	int v = gpio_pin_get_dt(&button);
	if (v < 0) {
		LOG_ERR("Falha ao ler botao: %d", v);
		return false;
	}
	bool pressed = (v != 0);
	if ((button.dt_flags & GPIO_ACTIVE_LOW) != 0U) {
		pressed = !pressed;
	}
	return pressed;
}

static bool button_was_pressed(void)
{
	static bool last_state;
	static int64_t last_transition_ms;
	const bool current_state = read_button_pressed();
	const int64_t now = k_uptime_get();
	if (current_state != last_state && (now - last_transition_ms) >= CONFIG_BUTTON_DEBOUNCE_MS) {
		last_state = current_state;
		last_transition_ms = now;
		return current_state;
	}
	return false;
}

int main(void)
{
	int err;

	if (!device_is_ready(led_gpio.port)) {
		LOG_ERR("GPIO do LED indisponivel");
		return -ENODEV;
	}

	if (!device_is_ready(led_pwm.dev)) {
		LOG_ERR("Controlador PWM indisponivel");
		return -ENODEV;
	}

	if (!device_is_ready(button.port)) {
		LOG_ERR("GPIO do botao indisponivel");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err < 0) {
		LOG_ERR("Falha ao configurar botao: %d", err);
		return err;
	}

	err = gpio_pin_configure_dt(&led_gpio, GPIO_OUTPUT_INACTIVE);
	if (err < 0) {
		LOG_ERR("Falha ao configurar LED: %d", err);
		return err;
	}

	err = pwm_set_dt(&led_pwm, CONFIG_LED_PWM_PERIOD_US, 0U);
	if (err < 0) {
		LOG_ERR("Falha ao inicializar PWM: %d", err);
		return err;
	}

	LOG_INF("Atividade 2 inicializada. Modo padrao: GPIO");

	bool led_on = false;
	uint32_t pwm_pulse_us = 0U;
	int pwm_direction = 1;
	int current_mode = LED_MODE_GPIO;

	int last_mode = current_mode;
	int64_t next_gpio_toggle_ms = k_uptime_get() + CONFIG_LED_BLINK_INTERVAL_MS;
	int64_t next_pwm_step_ms = k_uptime_get() + CONFIG_LED_PWM_STEP_INTERVAL_MS;

	while (1) {
		const int64_t now = k_uptime_get();
		const bool pressed = button_was_pressed();

		if (pressed) {
			current_mode = (current_mode + 1) % LED_MODE_COUNT;
			LOG_INF("Botao pressionado -> modo %s", (current_mode == LED_MODE_GPIO) ? "GPIO" : "PWM");
		}

		if (current_mode != last_mode) {
			if (current_mode == LED_MODE_GPIO) {
				pwm_apply(0U);
				gpio_pin_set_dt(&led_gpio, 0);
				led_on = false;
				LOG_INF("Modo GPIO ativo: piscando LED");
				next_gpio_toggle_ms = now + CONFIG_LED_BLINK_INTERVAL_MS;
			} else {
				gpio_pin_set_dt(&led_gpio, 0);
				pwm_pulse_us = 0U;
				pwm_direction = 1;
				pwm_apply(pwm_pulse_us);
				LOG_INF("Modo PWM ativo: fade in/out");
				next_pwm_step_ms = now + CONFIG_LED_PWM_STEP_INTERVAL_MS;
			}
			last_mode = current_mode;
		}

		if (current_mode == LED_MODE_GPIO) {
			if (now >= next_gpio_toggle_ms) {
				led_on = !led_on;
				err = gpio_pin_set_dt(&led_gpio, led_on);
				if (err < 0) {
					LOG_ERR("Falha ao alternar LED: %d", err);
				}
				next_gpio_toggle_ms = now + CONFIG_LED_BLINK_INTERVAL_MS;
			}
		} else {
			if (now >= next_pwm_step_ms) {
				if (pwm_direction > 0) {
					if ((CONFIG_LED_PWM_PERIOD_US - pwm_pulse_us) <= pwm_step_us) {
						pwm_pulse_us = CONFIG_LED_PWM_PERIOD_US;
						pwm_direction = -1;
					} else {
						pwm_pulse_us += pwm_step_us;
					}
				} else {
					if (pwm_pulse_us <= pwm_step_us) {
						pwm_pulse_us = 0U;
						pwm_direction = 1;
					} else {
						pwm_pulse_us -= pwm_step_us;
					}
				}
				pwm_apply(pwm_pulse_us);
				next_pwm_step_ms = now + CONFIG_LED_PWM_STEP_INTERVAL_MS;
			}
		}

		int sleep_ms = MIN(CONFIG_BUTTON_DEBOUNCE_MS, 10);
		if (sleep_ms <= 0) {
			sleep_ms = 5;
		}
		k_msleep(sleep_ms);
	}

	return 0;
}
