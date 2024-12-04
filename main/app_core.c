#include <app_core.h>
#include <app_av.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/xtensa_api.h"
#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s.h"
#include "freertos/ringbuf.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#define TAG "APP_CORE"

static void app_task_handler(void *arg);
static bool app_send_msg(app_msg_t *msg);
static void app_work_dispatched(app_msg_t *msg);
static uint8_t* col_data;
static uint8_t* audio_data;
RingbufHandle_t audio_channel;

xQueueHandle command_queue;
xQueueHandle s_app_task_queue;

xSemaphoreHandle cdat_semaphore;

xTaskHandle s_app_task_handle;
xTaskHandle s_i2s_task_handle;
xTaskHandle sensor_handle;
xTaskHandle command_handle;
xTaskHandle def_handle;
xTaskHandle color_handle;

bool STL_STATE = false;

bool app_work_dispatch(app_cb_t p_cback, uint16_t event, void *p_params, int param_len)
{
    ESP_LOGD(TAG, "%s event 0x%x, param len %d", __func__, event, param_len);

    app_msg_t msg;
    memset(&msg, 0, sizeof(app_msg_t));

    msg.sig = APP_SIG_WORK_DISPATCH;
    msg.event = event;
    msg.cb = p_cback;

    if (param_len == 0) {
        return app_send_msg(&msg);
    } else if (p_params && param_len > 0) {
        if ((msg.param = malloc(param_len)) != NULL) {
            memcpy(msg.param, p_params, param_len);
            return app_send_msg(&msg);
        }
    }

    return false;
}

static bool app_send_msg(app_msg_t *msg)
{
    if (msg == NULL) {
        return false;
    }

    if (xQueueSend(s_app_task_queue, msg, 10 / portTICK_RATE_MS) != pdTRUE) {
        ESP_LOGE(TAG, "%s xQueue send failed", __func__);
        return false;
    }
    return true;
}

static void app_work_dispatched(app_msg_t *msg)
{
    if (msg->cb) {
        msg->cb(msg->event, msg->param);
    }
}

static void app_task_handler(void *arg)
{
    app_msg_t msg;
    for (;;) {
        if (pdTRUE == xQueueReceive(s_app_task_queue, &msg, (portTickType)portMAX_DELAY)) {
            ESP_LOGD(TAG, "%s, sig 0x%x, 0x%x", __func__, msg.sig, msg.event);
            switch (msg.sig) {
            case APP_SIG_WORK_DISPATCH:
                app_work_dispatched(&msg);
                break;
            default:
                ESP_LOGW(TAG, "%s, unhandled sig: %d", __func__, msg.sig);
                break;
            } // switch (msg.sig)

            if (msg.param) {
                free(msg.param);
            }
        }
    }
}

void app_task_start_up(void)
{
	col_data = malloc(CSIZE);
	audio_data = malloc(CSIZE);
	cdat_semaphore = xSemaphoreCreateBinary();
    s_app_task_queue = xQueueCreate(10, sizeof(app_msg_t));
	command_queue = xQueueCreate(10, 1);
    xTaskCreate(app_task_handler, "BtAppT", 8192, NULL, configMAX_PRIORITIES - 3, &s_app_task_handle);
    return;
}

void app_task_shut_down(void)
{
    if (s_app_task_handle) { vTaskDelete(s_app_task_handle); s_app_task_handle = NULL; }
    if (s_app_task_queue) { vQueueDelete(s_app_task_queue); s_app_task_queue = NULL; }
    if (command_queue) { vQueueDelete(command_queue); command_queue = NULL; }
    if (cdat_semaphore) { vSemaphoreDelete(cdat_semaphore); cdat_semaphore = NULL;}

    free(col_data);
    free(audio_data);
    ESP_LOGI(TAG, "APP Task has been shut down");
}

static void i2s_task_handler(void *arg)
{
    int i;
    float V = 0.25;
    uint32_t VOLUME = 0;
    uint8_t *data = NULL;
	size_t item_size = 0;
	size_t bytes_written = 0;

	while (true) {
		data = (uint8_t *)xRingbufferReceive(audio_channel, &item_size, 10 / portTICK_PERIOD_MS);
		xTaskNotifyWait(0, 0, &VOLUME, 0);
		V = (float)VOLUME / 25.0f;

		if (item_size > 0){
			memset(audio_data, 0, CSIZE);
			memcpy(audio_data, data, item_size);
			for(i=0; i<CSIZE; i+=2) {
			  *((int16_t *)(audio_data+i)) = *((int16_t *)(audio_data+i)) * V;
			}
			i2s_write(i2s_out_num, audio_data, item_size, &bytes_written, portMAX_DELAY);
			vRingbufferReturnItem(audio_channel,(void *)data);
		}
	}
}

void i2s_task_start_up(void)
{
	esp_err_t err;
	i2s_config_t i2s_config = {
		.mode = I2S_MODE_MASTER | I2S_MODE_TX,
		.sample_rate = 44100,
		.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
		.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
		.communication_format = I2S_COMM_FORMAT_STAND_I2S | I2S_COMM_FORMAT_STAND_MSB,
		.tx_desc_auto_clear = true,
		.dma_buf_count = 10,
		.dma_buf_len = 512,
		.use_apll = true,
		.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1 // @suppress("Symbol is not resolved")
	};

	i2s_pin_config_t pin_config = {
		.bck_io_num = DAC_BCK,
		.ws_io_num = DAC_WS,
		.data_out_num = DAC_DIN,
		.data_in_num = I2S_PIN_NO_CHANGE // @suppress("Symbol is not resolved")
	};

	err = i2s_driver_install(i2s_out_num, &i2s_config, 0, NULL);
	if(err != ESP_OK){
		ESP_LOGI(TAG, "I2S Driver install failed");
		return;
	}
	err = i2s_set_pin(i2s_out_num, &pin_config);
	if(err != ESP_OK){
		ESP_LOGI(TAG, "I2S pin configuration failed");
		return;
	}
    audio_channel = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
    if(audio_channel == NULL){
    	ESP_LOGE(TAG, "Can't Allocate required channel.");
        return;
    }

    xTaskCreate(i2s_task_handler, "BtI2ST", 6144, NULL, tskIDLE_PRIORITY, &s_i2s_task_handle);
    return;
}

void cmpl_tasks_start_up(uint16_t event, void *param){
	xTaskCreate(process_colors, "color_task", 20480, col_data, tskIDLE_PRIORITY, &color_handle);
	xTaskCreate(sensor_task, "sensor_task", 2048, NULL, 3, &sensor_handle);
	xTaskCreate(cmd_cb_task, "cmd_task", 3072, NULL, 1, &command_handle);
	xTaskCreate(play_default, "default_task", 8192, NULL, 5, &def_handle);
}

void cmpl_tasks_shut_down(uint16_t event, void *param){
	if(def_handle != NULL) {xTaskNotify(def_handle, ABORT, eSetValueWithOverwrite);}
	if(color_handle != NULL) {xTaskNotify(color_handle, ABORT, eSetValueWithOverwrite);}
	if(command_handle != NULL) {xTaskNotify(command_handle, ABORT, eSetValueWithOverwrite);}
	if(sensor_handle != NULL) {xTaskNotify(sensor_handle, ABORT, eSetValueWithOverwrite);}
}

void i2s_task_shut_down(void)
{
    if (s_i2s_task_handle) {
        vTaskDelete(s_i2s_task_handle);
        s_i2s_task_handle = NULL;
    }

    if (audio_channel) {
        vRingbufferDelete(audio_channel);
        audio_channel = NULL;
    }

    i2s_driver_uninstall(i2s_out_num);
    ESP_LOGI(TAG, "I2S Task has been shut down");
}


void write_ringbuf(const uint8_t *data, size_t size)
{
	if(STL_STATE || OVL_STATE) return;

	if(size < CSIZE){
		memcpy(col_data, data, size);
		memset(col_data + size, 0, CSIZE - size);
	}else{
		memcpy(col_data, data, CSIZE);
	}
	xSemaphoreGive(cdat_semaphore);
	xRingbufferSend(audio_channel, (void *)data, size, (portTickType)portMAX_DELAY);
}



