#ifndef __APP_CORE_H__
#define __APP_CORE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include "driver/dac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"

//--------------------- PIN CONFIG -------------------------------

#define SD_MODULE_CS 						GPIO_NUM_5
#define SD_MODULE_SCLK 						GPIO_NUM_18
#define SD_MODULE_MOSI 						GPIO_NUM_23
#define SD_MODULE_MISO 						GPIO_NUM_19

#define DAC_WS 								GPIO_NUM_4
#define DAC_DIN 							GPIO_NUM_16
#define DAC_BCK 							GPIO_NUM_17

#define SERVO_1 							GPIO_NUM_2
#define SERVO_2 							GPIO_NUM_15
#define SERVO_3 							GPIO_NUM_21
#define SERVO_4 							GPIO_NUM_22

#define WS2812B_DOUT 						GPIO_NUM_12
#define VOLTAGE_SENSOR 						ADC2_CHANNEL_7
#define CHARGER_DETECT						ADC1_CHANNEL_4

#define NEON_1 								DAC_CHANNEL_1
#define NEON_2								DAC_CHANNEL_2


// ----------------------------------------------------------------

#define CONTROLLER_CONNECTED 				51
#define CONTROLLER_DISCONNECTED 			65
#define VOLUME_CHANGE 						35
#define NO_MODE 							0
#define DEFAULT_MODE 						10
#define BLUETOOTH_MODE 						15
#define LIGHT_ON							20
#define LIGHT_OFF							25
#define COMMAND_MODE_ACTIVE 				100
#define COMMAND_MODE_ACCEPTED 				101
#define COMMAND_MODE_INACTIVE 				102

#define CHUNK_SIZE 							1024
#define CSIZE	 							4096
#define HALF_CS 							512

#define CRITICAL_CHARGE_BOUND 				560
#define LOW_CHARGE_BOUND 					590

#define CRG_CONN							85
#define CRG_DISCONN							90
#define BATTERY_LOW 						11
#define GM_NARRATE_EVENT 					724
#define GN_NARRATE_EVENT 					750
#define BCS_NARRATE_EVENT 					782

#define N_LED 18
#define HN_LED 9

#define STOP_DEF									1011
#define START_DEF									1203
#define STOP_LGT									2101
#define START_LGT									2302
#define ABORT										3333

extern xSemaphoreHandle cdat_semaphore;

extern xQueueHandle s_app_task_queue;
extern xQueueHandle command_queue;
extern xTaskHandle s_i2s_task_handle;
extern xTaskHandle def_handle;
extern xTaskHandle color_handle;
extern xTaskHandle sensor_handle;
extern xTaskHandle command_handle;
extern RingbufHandle_t audio_channel;

extern bool STL_STATE;
extern bool OVL_STATE;
static const int i2s_out_num = 0;
extern uint16_t MODE;

#define APP_SIG_WORK_DISPATCH          (0x01)

typedef void (* app_cb_t) (uint16_t event, void *param);

typedef struct {
    uint16_t sig;
    uint16_t event;
    app_cb_t cb;
    void *param;
} app_msg_t;

bool app_work_dispatch(app_cb_t p_cback, uint16_t event, void *p_params, int param_len);

extern void app_task_start_up(void);
extern void app_task_shut_down(void);
extern void i2s_task_start_up(void);
extern void i2s_task_shut_down(void);
extern void cmpl_tasks_start_up(uint16_t event, void *param);
extern void cmpl_tasks_shut_down(uint16_t event, void *param);

extern void write_ringbuf(const uint8_t *data, size_t size);
extern void init_ext_storage();

extern void cmd_active(uint16_t event, void *param);
extern void set_mode(uint16_t event, void *param);
extern void change_volume(uint16_t event, void *param);
extern void indirect_narrate(uint16_t event, void *param);
extern void overlay_battery_status(uint16_t event, void *param);
extern void set_light(uint16_t event, void *param);

extern void play_default(void* param);
extern void process_colors(void *param);
extern void sensor_task(void *arg);
extern void cmd_cb_task(void *arg);

#endif /* __APP_CORE_H__ */
