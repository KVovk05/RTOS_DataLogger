#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_partition.h"
#include "env_spi.h"
#include "i2c_bus.h"
#include "rtc_ds1307.h"
#include "oled_display.h"

static const char *TAG = "RTOS_LOGGER";



#pragma pack(push, 1)
typedef struct {
    uint32_t timestamp;  
    int16_t  temp;       
    uint16_t hum;        
    uint16_t press;      
} log_record_t;
#pragma pack(pop)


typedef struct {
    log_record_t record;
    rtc_time_t rtc;
} display_msg_t;


QueueHandle_t sensor_queue;
QueueHandle_t display_queue;
QueueHandle_t flash_queue;
SemaphoreHandle_t i2c_mutex;
TaskHandle_t sensor_task_handle;

volatile bool is_flash_full = false;
#define SAVE_INTERVAL_SEC 60  // Зберігати у Flash раз на 60 секунд (для тесту)
#define SECTOR_SIZE 4096      // Розмір сектора Flash

static uint32_t rtc_to_seconds(rtc_time_t *t) {
    return (t->hour * 3600) + (t->min * 60) + t->sec;
}


void sensor_task(void *pvParameters) {
    float temp_c, press_hpa, hum_rh;
    rtc_time_t t;
    display_msg_t msg;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bool spi_ok = env_spi_read(&temp_c, &press_hpa, &hum_rh);

        bool rtc_ok = false;
        if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            rtc_ok = rtc_ds1307_read_time(&t);
            xSemaphoreGive(i2c_mutex);
        }

        if (spi_ok && rtc_ok) {
            msg.rtc = t;
            msg.record.timestamp = rtc_to_seconds(&t);
            msg.record.temp = (int16_t)(temp_c * 100);
            msg.record.hum = (uint16_t)(hum_rh * 100);
            msg.record.press = (uint16_t)(press_hpa * 0.75006f);

            xQueueSend(sensor_queue, &msg, 0);
        } else {
            ESP_LOGE(TAG, "Sensor or RTC read error");
        }
    }
}

 
void display_task(void *pvParameters) {
    display_msg_t msg;

    while (1) {
        
        if (xQueueReceive(display_queue, &msg, portMAX_DELAY)) {
            if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              
				  float t_c = msg.record.temp / 100.0f;
				  float h_rh = msg.record.hum / 100.0f;
				  float p_mm = msg.record.press;

				  oled_draw_dashboard(&msg.rtc, t_c, h_rh, p_mm, is_flash_full ? "STATUS: FLASH FULL!" : "STATUS: LOGGING"); 
				
                xSemaphoreGive(i2c_mutex);
            }
        }
    }
}


void flash_writer_task(void *pvParameters) {
    log_record_t record;
    static uint8_t ram_buffer[SECTOR_SIZE];
    uint32_t buffer_idx = 0;
    uint32_t flash_offset = 0;

    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "logdata");
    if (partition == NULL) {
        ESP_LOGE(TAG, "Partition 'logdata' not found! Check partitions.csv");
        vTaskDelete(NULL);
    }

    while (1) {
        if (xQueueReceive(flash_queue, &record, portMAX_DELAY)) {
            if (is_flash_full) continue; 

            
            memcpy(&ram_buffer[buffer_idx], &record, sizeof(log_record_t));
            buffer_idx += sizeof(log_record_t);

            if (buffer_idx + sizeof(log_record_t) > SECTOR_SIZE) {
                
                if (flash_offset + SECTOR_SIZE <= partition->size) {
                    esp_partition_erase_range(partition, flash_offset, SECTOR_SIZE);
                    esp_partition_write(partition, flash_offset, ram_buffer, buffer_idx);
                    ESP_LOGI(TAG, "Written %lu bytes to flash offset 0x%lx", buffer_idx, flash_offset);
                    
                    flash_offset += SECTOR_SIZE;
                    buffer_idx = 0;
                } else {
             
                    is_flash_full = true;
                    ESP_LOGW(TAG, "FLASH IS FULL. Logging stopped.");
                }
            }
        }
    }
}

void manager_task(void *pvParameters) {
    display_msg_t msg;
    int seconds_counter = 0;

  
    esp_sleep_enable_timer_wakeup(1000000);

    while (1) {
     
        xTaskNotifyGive(sensor_task_handle);

        if (xQueueReceive(sensor_queue, &msg, pdMS_TO_TICKS(500))) {
            
            xQueueSend(display_queue, &msg, 0);
            seconds_counter++;
            if (seconds_counter >= SAVE_INTERVAL_SEC) {
                xQueueSend(flash_queue, &msg.record, 0);
                seconds_counter = 0;
            }
        }
		vTaskDelay(pdMS_TO_TICKS(150));       
    }
}


void app_main(void)
{

    i2c_bus_init();
	env_spi_init();
    rtc_ds1307_init();
    oled_display_init();

    ESP_LOGI(TAG, "Showing boot wolf animation (~5s)...");
    oled_play_wolf_boot_animation();


    i2c_mutex = xSemaphoreCreateMutex();
    sensor_queue = xQueueCreate(5, sizeof(display_msg_t));
    display_queue = xQueueCreate(5, sizeof(display_msg_t));
    flash_queue = xQueueCreate(10, sizeof(log_record_t));

    if (i2c_mutex == NULL || sensor_queue == NULL || display_queue == NULL || flash_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create RTOS primitives");
        return;
    }

  
    xTaskCreate(sensor_task,       "sensor_task",  8192, NULL, 4, &sensor_task_handle);
    xTaskCreate(display_task,      "display_task", 8192, NULL, 3, NULL);
    xTaskCreate(flash_writer_task, "flash_task",   8192, NULL, 2, NULL);
    xTaskCreate(manager_task,      "manager_task", 8192, NULL, 5, NULL); 
}