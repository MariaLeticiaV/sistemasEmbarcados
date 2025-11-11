#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

enum sensor_type { SENSOR_TEMPERATURE = 0, SENSOR_HUMIDITY, SENSOR_TYPE_COUNT };
enum sensor_error { SENSOR_ERR_TOO_LOW = 0, SENSOR_ERR_TOO_HIGH };

struct sensor_reading {
	enum sensor_type type;
	int16_t value;
	int64_t timestamp_ms;
};

struct sensor_fault {
	struct sensor_reading sample;
	enum sensor_error error;
	int16_t min_expected;
	int16_t max_expected;
};

#define QUEUE_ALIGNMENT 4

K_MSGQ_DEFINE(input_queue, sizeof(struct sensor_reading), CONFIG_FILTER_QUEUE_DEPTH, QUEUE_ALIGNMENT);
K_MSGQ_DEFINE(output_queue, sizeof(struct sensor_reading), CONFIG_FILTER_QUEUE_DEPTH, QUEUE_ALIGNMENT);
K_MSGQ_DEFINE(error_queue, sizeof(struct sensor_fault), CONFIG_FILTER_QUEUE_DEPTH, QUEUE_ALIGNMENT);

#define PRODUCER_STACK_SIZE 1024
#define FILTER_STACK_SIZE 1024
#define CONSUMER_STACK_SIZE 1024
#define LOGGER_STACK_SIZE 1024
#define PRODUCER_PRIORITY 5
#define FILTER_PRIORITY 4
#define CONSUMER_PRIORITY 6
#define LOGGER_PRIORITY 6

static const int16_t temperature_pattern[] = { 21, 17, 19, 35, 28, 14, 30, 26 };
static const int16_t humidity_pattern[] = { 45, 32, 55, 68, 72, 60, 38, 66 };

static const char *const sensor_names[] = {
	[SENSOR_TEMPERATURE] = "Temperatura",
	[SENSOR_HUMIDITY] = "Umidade",
};

static void producer_thread(void *pattern_ptr, void *type_ptr, void *unused)
{
	const int16_t *pattern = pattern_ptr;
	const enum sensor_type type = POINTER_TO_INT(type_ptr);
	const size_t pattern_len = (type == SENSOR_TEMPERATURE) ? ARRAY_SIZE(temperature_pattern) : ARRAY_SIZE(humidity_pattern);
	size_t index = 0;
	const uint32_t sleep_ms = (type == SENSOR_TEMPERATURE) ? CONFIG_TEMP_PRODUCER_INTERVAL_MS : CONFIG_HUM_PRODUCER_INTERVAL_MS;

	ARG_UNUSED(unused);

	while (true) {
		struct sensor_reading sample = { .type = type, .value = pattern[index], .timestamp_ms = k_uptime_get() };
		if (k_msgq_put(&input_queue, &sample, K_FOREVER) == 0)
			LOG_DBG("%s produzida: %d", sensor_names[sample.type], sample.value);
		index = (index + 1U) % pattern_len;
		k_msleep(sleep_ms);
	}
}

static bool validate_reading(const struct sensor_reading *sample, int16_t *min_allowed, int16_t *max_allowed, enum sensor_error *error)
{
	switch (sample->type) {
	case SENSOR_TEMPERATURE:
		*min_allowed = CONFIG_TEMP_VALID_MIN_C;
		*max_allowed = CONFIG_TEMP_VALID_MAX_C;
		break;
	case SENSOR_HUMIDITY:
		*min_allowed = CONFIG_HUM_VALID_MIN_PCT;
		*max_allowed = CONFIG_HUM_VALID_MAX_PCT;
		break;
	default:
		*min_allowed = INT16_MIN;
		*max_allowed = INT16_MAX;
		break;
	}
	if (sample->value < *min_allowed) {
		*error = SENSOR_ERR_TOO_LOW;
		return false;
	}
	if (sample->value > *max_allowed) {
		*error = SENSOR_ERR_TOO_HIGH;
		return false;
	}
	return true;
}

static void filter_thread(void *, void *, void *)
{
	while (true) {
		struct sensor_reading sample;
		if (k_msgq_get(&input_queue, &sample, K_FOREVER) != 0)
			continue;
		int16_t min_allowed = 0, max_allowed = 0;
		enum sensor_error error_flag;
		bool valid = validate_reading(&sample, &min_allowed, &max_allowed, &error_flag);
		if (valid) k_msgq_put(&output_queue, &sample, K_FOREVER);
		else {
			struct sensor_fault fault = { .sample = sample, .error = error_flag, .min_expected = min_allowed, .max_expected = max_allowed };
			k_msgq_put(&error_queue, &fault, K_FOREVER);
		}
	}
}

static void consumer_thread(void *, void *, void *)
{
	while (true) {
		struct sensor_reading sample;
		if (k_msgq_get(&output_queue, &sample, K_FOREVER) == 0)
			LOG_INF("Consumidor recebeu %s = %d (t=%lld ms)", sensor_names[sample.type], sample.value, sample.timestamp_ms);
	}
}

static void error_logger_thread(void *, void *, void *)
{
	while (true) {
		struct sensor_fault fault;
		if (k_msgq_get(&error_queue, &fault, K_FOREVER) == 0) {
			const char *dir = (fault.error == SENSOR_ERR_TOO_LOW) ? "abaixo" : "acima";
			LOG_WRN("%s inconsistente (%s do esperado): valor=%d, faixa [%d, %d], t=%lld ms",
				sensor_names[fault.sample.type], dir, fault.sample.value,
				fault.min_expected, fault.max_expected, fault.sample.timestamp_ms);
		}
	}
}

K_THREAD_DEFINE(temp_producer_id, PRODUCER_STACK_SIZE, producer_thread, (void *)temperature_pattern, INT_TO_POINTER(SENSOR_TEMPERATURE), NULL, PRODUCER_PRIORITY, 0, 0);
K_THREAD_DEFINE(hum_producer_id, PRODUCER_STACK_SIZE, producer_thread, (void *)humidity_pattern, INT_TO_POINTER(SENSOR_HUMIDITY), NULL, PRODUCER_PRIORITY, 0, 0);
K_THREAD_DEFINE(filter_id, FILTER_STACK_SIZE, filter_thread, NULL, NULL, NULL, FILTER_PRIORITY, 0, 0);
K_THREAD_DEFINE(consumer_id, CONSUMER_STACK_SIZE, consumer_thread, NULL, NULL, NULL, CONSUMER_PRIORITY, 0, 0);
K_THREAD_DEFINE(logger_id, LOGGER_STACK_SIZE, error_logger_thread, NULL, NULL, NULL, LOGGER_PRIORITY, 0, 0);

int main(void)
{
	LOG_INF("Sistema de estufa inteligente inicializado");
	while (true) k_sleep(K_FOREVER);
	return 0;
}
