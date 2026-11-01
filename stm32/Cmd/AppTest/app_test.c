/*
 * Тест логера і supervisor на платі.
 *
 * Що запускається:
 *   Logger        - черга 16 подій, рядок формує таска логера, UART через DMA
 *   Supervisor    - перевірка раз на 500 мс, годує IWDG
 *   demo_task     - кожні 200 мс пише лог і відмічається; раз на секунду "тригерить" ISR, який теж пише лог
 *   burst_task    - раз на 5 с пише 40 повідомлень підряд (перевірка черги, склеювання в DMA і лічильника втрат)
 *   stack_hog     - лише в сценарії 2: займає майже весь свій стек
 */
#include "app_test.h"

#include "main.h"
#include "logger.h"
#include "supervisor.h"
#include "task.h"

/* 0 - норма, 1 - demo_task зависає, 2 - stack_hog майже вичерпує стек */
#ifndef APP_TEST_SCENARIO
#define APP_TEST_SCENARIO 0
#endif

/* Сценарій 1: через скільки секунд demo_task "зависає" */
#define APP_TEST_HANG_AFTER_S 15U

/* Сценарій 2: скільки байт займає локальний масив у stack_hog (підберіть під свій порт/оптимізацію) */
#define APP_TEST_STACK_HOG_BYTES 560U

/* 1 - визначити HAL-колбеки та хук переповнення стеку тут.
 * 0 - якщо вони вже є у вашому коді: тоді викличте з них Logger_UART_*Callback / Supervisor_StackOverflowHook. */
#define APP_TEST_DEFINE_HOOKS 1

/* 1 - IWDG не рахує, поки ядро зупинене в дебагері (щоб не скидало на breakpoint) */
#define APP_TEST_FREEZE_IWDG_ON_DEBUG 1

/* Пріоритети: logger < демо-таски < supervisor */
#define PRIO_LOGGER     (tskIDLE_PRIORITY + 1U)
#define PRIO_APP        (tskIDLE_PRIORITY + 2U)
#define PRIO_SUPERVISOR (tskIDLE_PRIORITY + 3U)

static SupervisorTaskID s_demo_id = SUPERVISOR_INVALID_ID;
static SupervisorTaskID s_burst_id = SUPERVISOR_INVALID_ID;
static SupervisorTaskID s_hog_id = SUPERVISOR_INVALID_ID;

/* ------------------------------------------------------------------------- */

/* Переривання для перевірки логування з ISR: запускається програмно через NVIC.
 * FPU_IRQn рідко використовується, його обробник у startup-файлі слабкий (weak). */
void FPU_IRQHandler(void) {
	LOG_I("isr: hello from interrupt");
}

static void demo_task(void *arg) {
	(void)arg;
	uint32_t n = 0;

	for (;;) {
		n++;
		LOG_I("demo alive, n=%lu", (unsigned long)n);

		if ((n % 25U) == 0U) {
			LOG_W("demo warning, n=%lu", (unsigned long)n);
		}
		if ((n % 50U) == 0U) {
			LOG_E("demo error, dropped so far=%lu", (unsigned long)Logger_GetDroppedCount());
		}
		if ((n % 5U) == 0U) {
			NVIC_SetPendingIRQ(FPU_IRQn);	/* раз на секунду -> FPU_IRQHandler() */
		}

		Supervisor_CheckIn(s_demo_id);

#if APP_TEST_SCENARIO == 1
		if (n == (APP_TEST_HANG_AFTER_S * 5U)) {
			LOG_W("demo: simulating hang, no more check-ins");
			vTaskDelay(portMAX_DELAY);	/* таска жива, але більше не відмічається */
		}
#endif
		vTaskDelay(pdMS_TO_TICKS(200));
	}
}

static void burst_task(void *arg) {
	(void)arg;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(5000));

		/* 40 повідомлень підряд при черзі на 16: частина має бути відкинута */
		for (int i = 0; i < 40; i++) {
			LOG_D("burst %d", i);
		}

		vTaskDelay(pdMS_TO_TICKS(300));	/* даємо логеру спорожнити чергу, щоб наступний рядок не відкинувся */
		LOG_I("burst done, dropped so far=%lu", (unsigned long)Logger_GetDroppedCount());

		Supervisor_CheckIn(s_burst_id);
	}
}

#if APP_TEST_SCENARIO == 2
static void stack_hog_task(void *arg) {
	(void)arg;

	for (;;) {
		volatile uint8_t buf[APP_TEST_STACK_HOG_BYTES];
		for (uint32_t i = 0; i < sizeof(buf); i++) {
			buf[i] = 0xAB;	/* volatile: компілятор не прибере цей запис */
		}
		Supervisor_CheckIn(s_hog_id);
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
#endif

/* ------------------------------------------------------------------------- */

void AppTest_Start(const AppTestConfig *cfg) {
#if APP_TEST_FREEZE_IWDG_ON_DEBUG
	__HAL_DBGMCU_FREEZE_IWDG();
#endif

	/* 1. Логер першим: усе інше пише через нього (зокрема причину скидання). */
	const LoggerConfig log_cfg = {
		.min_log_level = LOG_DEBUG,
		.huart = cfg->huart,
		.queue_length = 16,
		.max_message_length = 128,
		.task_stack_words = 384,	/* таска логера викликає printf */
		.task_priority = PRIO_LOGGER,
	};
	const Result(LoggerError, LoggerError) log_res = Logger_Init(&log_cfg);
	if (log_res.is_error) {
		Error_Handler();
	}
	LOG_I("=== app_test start, scenario %d ===", APP_TEST_SCENARIO);

	/* 2. Supervisor. Таймаут IWDG у CubeMX має бути ~2 с (більше за період 500 мс). */
	const SupervisorConfig sup_cfg = {
		.hiwdg = cfg->hiwdg,
		.check_period_ms = 500,
		.stack_min_free_words = 32,
		.task_stack_words = 256,	/* LOG_x дешевий: рядок формує таска логера */
		.task_priority = PRIO_SUPERVISOR,
	};
	const Result(SupervisorError, SupervisorError) sup_res = Supervisor_Init(&sup_cfg);
	if (sup_res.is_error) {
		Error_Handler();
	}

	/* 3. Лог з ISR: пріоритет має бути не вищим за configMAX_SYSCALL_INTERRUPT_PRIORITY. */
	NVIC_SetPriority(FPU_IRQn, (configMAX_SYSCALL_INTERRUPT_PRIORITY >> (8U - __NVIC_PRIO_BITS)) + 1U);
	NVIC_EnableIRQ(FPU_IRQn);

	/* 4. Таски і їх реєстрація в supervisor (до старту планувальника ще можна). */
	TaskHandle_t h = NULL;

	if (xTaskCreate(demo_task, "Demo", 256, NULL, PRIO_APP, &h) != pdPASS) {
		Error_Handler();
	}
	s_demo_id = Supervisor_Register(h, 1000);		/* має відмічатись щонайменше раз на секунду */

	if (xTaskCreate(burst_task, "Burst", 256, NULL, PRIO_APP, &h) != pdPASS) {
		Error_Handler();
	}
	s_burst_id = Supervisor_Register(h, 8000);	/* цикл 5.3 с, запас до 8 с */

#if APP_TEST_SCENARIO == 2
	if (xTaskCreate(stack_hog_task, "StackHog", 192, NULL, PRIO_APP, &h) != pdPASS) {
		Error_Handler();
	}
	s_hog_id = Supervisor_Register(h, 3000);
#endif
	(void)s_hog_id;
}

/* ------------------------------------------------------------------------- */

#if APP_TEST_DEFINE_HOOKS
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
	Logger_UART_TxCpltCallback(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
	Logger_UART_ErrorCallback(huart);
}

/* Потрібно configCHECK_FOR_STACK_OVERFLOW = 2 у FreeRTOSConfig.h */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
	Supervisor_StackOverflowHook(xTask, pcTaskName);
}
#endif
