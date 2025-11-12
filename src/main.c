#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define LED_GPIO_NODE DT_ALIAS(led0)
#define LED_PWM_NODE DT_ALIAS(pwm_led0)
#define BUTTON_NODE DT_ALIAS(sw0)

BUILD_ASSERT(DT_NODE_HAS_STATUS(LED_GPIO_NODE, okay), "led0 indefinido");
BUILD_ASSERT(DT_NODE_HAS_STATUS(LED_PWM_NODE, okay), "pwm_led0 indefinido");
BUILD_ASSERT(DT_NODE_HAS_STATUS(BUTTON_NODE, okay), "sw0 indefinido");

static const struct gpio_dt_spec led_gpio = GPIO_DT_SPEC_GET(LED_GPIO_NODE, gpios);
static const struct pwm_dt_spec led_pwm = PWM_DT_SPEC_GET(LED_PWM_NODE);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);

enum mode { GPIO_MODE, PWM_MODE };

static const uint32_t pwm_step = MAX(1U, (CONFIG_LED_PWM_PERIOD_US * CONFIG_LED_PWM_STEP_PCT) / 100U);

static int init_devices(void) {
	if (!device_is_ready(led_gpio.port) || !device_is_ready(led_pwm.dev) || !device_is_ready(button.port))
		return -ENODEV;
	int r = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (r < 0) return r;
	r = gpio_pin_configure_dt(&led_gpio, GPIO_OUTPUT_INACTIVE);
	if (r < 0) return r;
	r = pwm_set_dt(&led_pwm, CONFIG_LED_PWM_PERIOD_US, 0U);
	return r;
}

static bool read_button(void) {
	int v = gpio_pin_get_dt(&button);
	if (v < 0) return false;
	bool pressed = (v != 0);
	if ((button.dt_flags & GPIO_ACTIVE_LOW) != 0U) pressed = !pressed;
	return pressed;
}

static bool button_pressed(void) {
	static bool last;
	static int64_t last_t;
	bool curr = read_button();
	int64_t now = k_uptime_get();
	if (curr != last && (now - last_t) >= CONFIG_BUTTON_DEBOUNCE_MS) {
		last = curr;
		last_t = now;
		return curr;
	}
	return false;
}

static void set_pwm(uint32_t pulse) {
	if (pwm_set_dt(&led_pwm, CONFIG_LED_PWM_PERIOD_US, pulse) < 0)
		LOG_ERR("Erro PWM");
}

int main(void) {
	if (init_devices() < 0) return -ENODEV;
	LOG_INF("Atividade inicializada");
	int mode = GPIO_MODE;
	bool led_on = false;
	uint32_t pulse = 0;
	int dir = 1;
	int64_t next_gpio = k_uptime_get() + CONFIG_LED_BLINK_INTERVAL_MS;
	int64_t next_pwm = k_uptime_get() + CONFIG_LED_PWM_STEP_INTERVAL_MS;

	while (1) {
		int64_t now = k_uptime_get();
		if (button_pressed()) {
			mode = (mode == GPIO_MODE) ? PWM_MODE : GPIO_MODE;
			LOG_INF("Modo %s", mode == GPIO_MODE ? "GPIO" : "PWM");
			if (mode == GPIO_MODE) {
				set_pwm(0);
				gpio_pin_set_dt(&led_gpio, 0);
				next_gpio = now + CONFIG_LED_BLINK_INTERVAL_MS;
			} else {
				gpio_pin_set_dt(&led_gpio, 0);
				pulse = 0;
				dir = 1;
				next_pwm = now + CONFIG_LED_PWM_STEP_INTERVAL_MS;
			}
		}

		if (mode == GPIO_MODE) {
			if (now >= next_gpio) {
				led_on = !led_on;
				gpio_pin_set_dt(&led_gpio, led_on);
				next_gpio = now + CONFIG_LED_BLINK_INTERVAL_MS;
			}
		} else {
			if (now >= next_pwm) {
				pulse += dir * pwm_step;
				if (pulse >= CONFIG_LED_PWM_PERIOD_US || pulse <= 0) {
					dir = -dir;
					pulse = CLAMP(pulse, 0, CONFIG_LED_PWM_PERIOD_US);
				}
				set_pwm(pulse);
				next_pwm = now + CONFIG_LED_PWM_STEP_INTERVAL_MS;
			}
		}
		k_msleep(MIN(CONFIG_BUTTON_DEBOUNCE_MS, 10));
	}
}
