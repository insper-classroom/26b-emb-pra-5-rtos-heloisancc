/*
 * Ultrassom HC-SR04 com FreeRTOS + OLED + LED RGB (Pico Dock)
 */
#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <stdio.h>
#include <task.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include "pins.h"
#include "ssd1306.h"

// === Pinos do ultrassom (confira com o seu circuito / pins.h) ===
const int ULTRA_TRIG_PIN = 16;
const int ULTRA_ECHO_PIN = 17;

// === Recursos do RTOS ===
QueueHandle_t xQueueTime;
QueueHandle_t xQueueDistance;
SemaphoreHandle_t xSemaphoreTrigger;

// === Callback do pino ECHO ===
void pin_callback(uint gpio, uint32_t events) {
    if (gpio == ULTRA_ECHO_PIN) {
        if (events & (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL)) {
            uint64_t t = to_us_since_boot(get_absolute_time());
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(xQueueTime, &t, &xHigherPriorityTaskWoken);
        }
    }
}

// === Inicializações ===
void oled_display_init(ssd1306_t *disp) {
    i2c_init(i2c1, 400000);
    gpio_set_function(2, GPIO_FUNC_I2C);
    gpio_set_function(3, GPIO_FUNC_I2C);
    gpio_pull_up(2);
    gpio_pull_up(3);

    disp->external_vcc = false;
    ssd1306_init(disp, 128, 64, 0x3C, i2c1);
    ssd1306_clear(disp);
    ssd1306_show(disp);

    gpio_init(SSD1306_PIN_LITE);
    gpio_set_dir(SSD1306_PIN_LITE, GPIO_OUT);
    gpio_put(SSD1306_PIN_LITE, 0);
}

void led_rgb_init(void) {
    gpio_init(LED_PIN_R);
    gpio_set_dir(LED_PIN_R, GPIO_OUT);
    gpio_put(LED_PIN_R, 1);

    gpio_init(LED_PIN_G);
    gpio_set_dir(LED_PIN_G, GPIO_OUT);
    gpio_put(LED_PIN_G, 1);

    gpio_init(LED_PIN_B);
    gpio_set_dir(LED_PIN_B, GPIO_OUT);
    gpio_put(LED_PIN_B, 1);
}

// LED RGB da Pico Dock é ativo em 0
void led_rgb_set(int r, int g, int b) {
    gpio_put(LED_PIN_R, !r);
    gpio_put(LED_PIN_G, !g);
    gpio_put(LED_PIN_B, !b);
}

// === Tasks ===
void trigger_task(void *p) {
    gpio_init(ULTRA_TRIG_PIN);
    gpio_set_dir(ULTRA_TRIG_PIN, GPIO_OUT);
    gpio_put(ULTRA_TRIG_PIN, 0);

    while (true) {
        gpio_put(ULTRA_TRIG_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1)); // pulso de trigger (liberado pelo enunciado)
        gpio_put(ULTRA_TRIG_PIN, 0);

        xSemaphoreGive(xSemaphoreTrigger);

        vTaskDelay(pdMS_TO_TICKS(200)); // intervalo entre leituras
    }
}

void echo_task(void *p) {
    gpio_init(ULTRA_ECHO_PIN);
    gpio_set_dir(ULTRA_ECHO_PIN, GPIO_IN);
    gpio_set_irq_enabled_with_callback(ULTRA_ECHO_PIN,
                                       GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
                                       true, &pin_callback);

    uint64_t t_rise = 0;
    uint64_t t_fall = 0;

    while (true) {
        if (xQueueReceive(xQueueTime, &t_rise, portMAX_DELAY) == pdTRUE) {
            // pulso do echo dura no máximo ~38 ms
            if (xQueueReceive(xQueueTime, &t_fall, pdMS_TO_TICKS(50)) == pdTRUE) {
                float distance = (float)(t_fall - t_rise) * 0.0343f / 2.0f;
                xQueueSend(xQueueDistance, &distance, 0);
            }
        }
    }
}

void oled_task(void *p) {
    ssd1306_t disp;
    oled_display_init(&disp);
    led_rgb_init();

    float distance = 0;
    char buf[32];

    while (true) {
        if (xSemaphoreTake(xSemaphoreTrigger, portMAX_DELAY) == pdTRUE) {
            ssd1306_clear(&disp);

            if (xQueueReceive(xQueueDistance, &distance, pdMS_TO_TICKS(100)) == pdTRUE) {
                // leitura OK
                snprintf(buf, sizeof(buf), "Dist: %.1f cm", distance);
                ssd1306_draw_string(&disp, 0, 0, 1, buf);

                // barra proporcional (0 a 400 cm -> 0 a 128 px)
                int bar = (int)(distance * 128.0f / 400.0f);
                if (bar > 128) {
                    bar = 128;
                }
                if (bar > 0) {
                    ssd1306_draw_square(&disp, 0, 30, bar, 10);
                }

                if (distance <= 100.0f) {
                    led_rgb_set(0, 1, 0); // verde
                } else {
                    led_rgb_set(1, 1, 0); // amarelo
                }
            } else {
                // dado não chegou: falha no sensor
                ssd1306_draw_string(&disp, 0, 0, 2, "FALHA");
                ssd1306_draw_string(&disp, 0, 24, 1, "Sensor sem resposta");
                led_rgb_set(1, 0, 0); // vermelho
            }

            ssd1306_show(&disp);
        }
    }
}

int main() {
    stdio_init_all();

    xQueueTime = xQueueCreate(32, sizeof(uint64_t));
    xQueueDistance = xQueueCreate(32, sizeof(float));
    xSemaphoreTrigger = xSemaphoreCreateBinary();

    xTaskCreate(trigger_task, "Trigger", 256, NULL, 1, NULL);
    xTaskCreate(echo_task, "Echo", 256, NULL, 1, NULL);
    xTaskCreate(oled_task, "OLED", 4095, NULL, 1, NULL);

    vTaskStartScheduler();

    while (true);
}