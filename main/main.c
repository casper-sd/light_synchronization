#include <app_core.h>
#include <app_av.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_spp_api.h"

#include "driver/i2s.h"
#include "esp_log.h"
#include "time.h"
#include "sys/time.h"
#include "driver/rmt.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "iot_servo.h"
#include "freertos/event_groups.h"

static const char* TAG = "MAIN";

#define DEVICE_NAME "SpecBox"
#define WAKEUP_BIT ( 1 << 2)
#define SLEEP_BIT ( 1 << 1 )
#define FORCE_SHUTDOWN_BIT ( 1 << 0 )

static EventGroupHandle_t xEventGroup;
static uint32_t cntrl_handle;
static esp_bd_addr_t controller_mac_addr;

void sensor_task(void *arg){
	ESP_LOGI(TAG, "Executing: %s", __func__);
	int reading = 0, C = 0;
	uint32_t ins = 0;
	uint8_t lc = 0;
	bool crg = false;
	uint16_t curr_charge = 1023;
	adc2_config_channel_atten(VOLTAGE_SENSOR, ADC_ATTEN_DB_11);
	adc1_config_channel_atten(CHARGER_DETECT, ADC_ATTEN_DB_6);
	adc1_config_width(ADC_WIDTH_BIT_9);
	while(ins != ABORT){
		C = adc1_get_raw(CHARGER_DETECT);
		if(!crg && C > 256){
			crg = true;
			curr_charge = 1023;
			app_work_dispatch(overlay_battery_status, CRG_CONN, NULL, 0);
		}
		else if(crg && C < 256){
			crg = false;
			app_work_dispatch(overlay_battery_status, CRG_DISCONN, NULL, 0);
		}
		if(!crg){
			adc2_get_raw(VOLTAGE_SENSOR, ADC_WIDTH_BIT_10, &reading);
			if(reading < curr_charge){
				curr_charge = reading;
			}
			if (curr_charge <= CRITICAL_CHARGE_BOUND){
				xEventGroupSetBits(xEventGroup, FORCE_SHUTDOWN_BIT);
			}
			else if(curr_charge <= LOW_CHARGE_BOUND){
				if(lc == 0) {
					app_work_dispatch(overlay_battery_status, BATTERY_LOW, NULL, 0);
					lc = 6;
				}
				else lc -= 1;
			}
		}
		xTaskNotifyWait(0, 0xffffffff, &ins, pdMS_TO_TICKS(10000));
	}
	sensor_handle = NULL;
	ESP_LOGI(TAG, "Stopped %s", __func__);
	vTaskDelete(NULL);
}

void cmd_cb_task(void *arg){
	ESP_LOGI(TAG, "Executing: %s", __func__);
	uint8_t command = 0;
	uint32_t ins = 0;
	bool cmd_accept = false;
	uint8_t active_count = 0, level = 5;
	while(ins != ABORT){
		if(xQueueReceive(command_queue, &command, 0) == pdTRUE){
			ESP_LOGI(TAG, "Received: %d", command);
			switch(command){
			case COMMAND_MODE_ACTIVE:
				app_work_dispatch(cmd_active, COMMAND_MODE_ACTIVE, NULL, 0);
				cmd_accept = true;
				active_count = 15;
				break;
			case DEFAULT_MODE:
			case BLUETOOTH_MODE:
				if(cmd_accept){
					cmd_accept = false;
					app_work_dispatch(cmd_active, COMMAND_MODE_ACCEPTED, NULL, 0);
					app_work_dispatch(set_mode, command, (void*)controller_mac_addr, ESP_BD_ADDR_LEN);
					active_count = 0;
				}
				break;
			case VOLUME_CHANGE:
				if(cmd_accept){
					cmd_accept = false;
					app_work_dispatch(cmd_active, COMMAND_MODE_ACCEPTED, NULL, 0);
					xQueueReceive(command_queue, &level, portMAX_DELAY);
					app_work_dispatch(change_volume, level, NULL, 0);
					active_count = 0;
				}
				break;
			case LIGHT_ON:
			case LIGHT_OFF:
				if(cmd_accept){
					cmd_accept = false;
					app_work_dispatch(cmd_active, COMMAND_MODE_ACCEPTED, NULL, 0);
					app_work_dispatch(set_light, command, NULL, 0);
					active_count = 0;
				}
				break;
			}
		}

		if(active_count > 0){
			active_count -= 1;
		}
		else if(cmd_accept){
			cmd_accept = false;
			app_work_dispatch(cmd_active, COMMAND_MODE_INACTIVE, NULL, 0);
		}
		xTaskNotifyWait(0, 0xffffffff, &ins, pdMS_TO_TICKS(1000));
	}
	command_handle = NULL;
	ESP_LOGI(TAG, "Stopped %s", __func__);
	vTaskDelete(NULL);
}


static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        ESP_LOGI(TAG, "ESP_SPP_INIT_EVT");
        esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, DEVICE_NAME);
        break;
    case ESP_SPP_CLOSE_EVT:
    	ESP_LOGI(TAG, "ESP_SPP_CLOSE_EVT");
    	xEventGroupSetBits(xEventGroup, SLEEP_BIT);
    	esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        break;
    case ESP_SPP_START_EVT:
        ESP_LOGI(TAG, "ESP_SPP_START_EVT");
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        break;
    case ESP_SPP_DATA_IND_EVT:
    	for(int i = 0; i < param->data_ind.len; i++){
    		xQueueSend(command_queue, param->data_ind.data + i, portMAX_DELAY);
    	}
        break;
    case ESP_SPP_SRV_OPEN_EVT:
    	cntrl_handle = param->srv_open.handle;
    	memcpy(controller_mac_addr, param->srv_open.rem_bda, ESP_BD_ADDR_LEN);
    	xEventGroupSetBits(xEventGroup, WAKEUP_BIT);
    	esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        ESP_LOGI(TAG, "ESP_SPP_SRV_OPEN_EVT");
        break;
    default:
        break;
    }
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:{
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "authentication success: %s", param->auth_cmpl.device_name);
            esp_log_buffer_hex(TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT:{
        ESP_LOGI(TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGI(TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGI(TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '8';
            pin_code[1] = '3';
            pin_code[2] = '7';
            pin_code[3] = '6';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
        break;

    default: {
        ESP_LOGI(TAG, "event: %d", event);
        break;
    }
    }
    return;
}

void app_main(void)
{
	/////////////////////////////////////////////////////////////////////////////////////////////

	nvs_flash_init();
	init_ext_storage();

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT(); // @suppress("Symbol is not resolved")

	esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
	esp_bt_controller_init(&bt_cfg);
	esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
	esp_bluedroid_init();
	assert(esp_bluedroid_enable() == ESP_OK);

	esp_bt_dev_set_device_name(DEVICE_NAME);
	esp_bt_gap_register_callback(esp_bt_gap_cb);

	esp_avrc_ct_init();
	esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
	assert (esp_avrc_tg_init() == ESP_OK);
	esp_avrc_tg_register_callback(bt_app_rc_tg_cb);

	esp_avrc_rn_evt_cap_mask_t evt_set = {0};
	esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
	esp_avrc_tg_set_rn_evt_cap(&evt_set);
	esp_a2d_register_callback(&bt_app_a2d_cb);
	esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);

	esp_spp_register_callback(esp_spp_cb);
	esp_spp_init(ESP_SPP_MODE_CB);

	////////////////////////////////////////////////////////////////////////////////////////////////

	servo_config_t servo_cfg = {
		.max_angle = 180,
		.min_width_us = 500,
		.max_width_us = 2500,
		.freq = 50,
		.timer_number = LEDC_TIMER_0,
		.channels = {
			.servo_pin = { SERVO_1, SERVO_2, SERVO_3, SERVO_4 },
			.ch = { LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3 },
		},
		.channel_number = 4,
	} ;
	xEventGroup = xEventGroupCreate();
	bool bcsn = false;
	EventBits_t uxBits;
	while(true){
		uxBits = xEventGroupWaitBits(xEventGroup,
				SLEEP_BIT | WAKEUP_BIT | FORCE_SHUTDOWN_BIT,
				pdTRUE, pdFALSE, portMAX_DELAY);
		if( (uxBits & WAKEUP_BIT) )
		{
			app_task_start_up();
			i2s_task_start_up();

			iot_servo_init(LEDC_LOW_SPEED_MODE, &servo_cfg);
			for(float i = 0; i <= 80.0f; i++){
				iot_servo_write_angle(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, i);
				iot_servo_write_angle(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, i);
				iot_servo_write_angle(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, i);
				iot_servo_write_angle(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, i);
				vTaskDelay(25 / portTICK_PERIOD_MS);
			}

			vTaskDelay(500 / portTICK_PERIOD_MS);
			xTaskNotify(s_i2s_task_handle, 5, eSetValueWithOverwrite);
			app_work_dispatch(indirect_narrate, GM_NARRATE_EVENT, NULL, 0);
			app_work_dispatch(cmpl_tasks_start_up, 0, NULL, 0);
		}
		else if( (uxBits & FORCE_SHUTDOWN_BIT) )
		{
			app_work_dispatch(set_mode, NO_MODE, (void*)controller_mac_addr, ESP_BD_ADDR_LEN);
			app_work_dispatch(indirect_narrate, BCS_NARRATE_EVENT, NULL, 0);
			esp_spp_disconnect(cntrl_handle);
			bcsn = true;
		}
		else if( (uxBits & SLEEP_BIT) )
		{
			app_work_dispatch(set_mode, NO_MODE, (void*)controller_mac_addr, ESP_BD_ADDR_LEN);
			if(!bcsn){
				app_work_dispatch(indirect_narrate, GN_NARRATE_EVENT, NULL, 0);
			}else bcsn = false;

			app_work_dispatch(cmpl_tasks_shut_down, 0, NULL, 0);

			for(float i = 80.0f; i >= 0.0f; i--){
				iot_servo_write_angle(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, i);
				iot_servo_write_angle(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, i);
				iot_servo_write_angle(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, i);
				iot_servo_write_angle(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, i);
				vTaskDelay(25 / portTICK_PERIOD_MS);
			}

			vTaskDelay(500 / portTICK_PERIOD_MS);
			iot_servo_deinit(LEDC_LOW_SPEED_MODE);

			app_msg_t msg;
			while(xQueuePeek(s_app_task_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) vTaskDelay(pdMS_TO_TICKS(500));

			vTaskDelay(2000 / portTICK_PERIOD_MS);
			i2s_task_shut_down();
			app_task_shut_down();
		}
	}

}

