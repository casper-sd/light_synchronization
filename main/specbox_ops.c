#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "app_core.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "driver/i2s.h"
#include "esp_bt_device.h"
#include "esp_a2dp_api.h"
#include "led_strip.h"
#include "esp_dsp.h"
#include "driver/rmt.h"

#define TAG "SPEC_OPS"
#define MOUNT_POINT "/sdcard"

#define MONOMAN						MOUNT_POINT"/monoman.wav"
#define GOODMORNING 				MOUNT_POINT"/gm.wav"
#define GOODNIGHT 					MOUNT_POINT"/gn.wav"
#define SWITCH_DEFAULT 				MOUNT_POINT"/def.wav"
#define SWITCH_BT 					MOUNT_POINT"/bt.wav"
#define CMD_YES_SOUND		 		MOUNT_POINT"/yes.wav"
#define CMD_ICGI_SOUND 				MOUNT_POINT"/icgi.wav"
#define CMD_GOTIT_SOUND 			MOUNT_POINT"/gotit.wav"
#define BATTERY_CRITICAL_SOUND 		MOUNT_POINT"/b_crit.wav"
#define BATTERY_LOW_SOUND 			MOUNT_POINT"/b_low.wav"
#define ADAPTER_CONN 				MOUNT_POINT"/ac.wav"
#define ADAPTER_DISC				MOUNT_POINT"/ad.wav"

uint16_t MODE = NO_MODE;
uint16_t LGT = LIGHT_OFF;
static const uint8_t BT_VOL = 5;
bool OVL_STATE = false;
static uint8_t narrate_data[CSIZE];

void init_ext_storage()
{
	esp_err_t ret;
	ESP_LOGI(TAG, "Initializing SD card");

	sdmmc_card_t *card;
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();

	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = false,
		.max_files = 5,
		.allocation_unit_size = 16 * 1024
	};


	spi_bus_config_t bus_cfg = {
		.mosi_io_num = SD_MODULE_MOSI,
		.miso_io_num = SD_MODULE_MISO,
		.sclk_io_num = SD_MODULE_SCLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4000,
	};

	host.slot = SPI3_HOST;
	ret = spi_bus_initialize(host.slot, &bus_cfg, 1);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to initialize bus.");
		return;
	}

	sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot_config.gpio_cs = SD_MODULE_CS;
	slot_config.host_id = host.slot;

	ESP_LOGI(TAG, "Mounting file system");
	ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

	if (ret != ESP_OK) {
		if (ret == ESP_FAIL) {
			ESP_LOGE(TAG, "Failed to mount filesystem. "
					 "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
		} else {
			ESP_LOGE(TAG, "Failed to initialize the card (%s). "
					 "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
		}
		return;
	}
	ESP_LOGI(TAG, "File system mounted");
}

static void narrate(const char* file)
{
	STL_STATE = true;
	vTaskDelay(200 / portTICK_PERIOD_MS);
	i2s_zero_dma_buffer(i2s_out_num);

	FILE* f = fopen(file, "r");
	if(f == NULL){
		ESP_LOGE(TAG, "Can't open: %s", file);
		return;
	}
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	rewind(f);
	uint32_t chunk = 0;
	size_t pos;

	uint32_t s_rate = i2s_get_clk(i2s_out_num);
	if(s_rate != 44100){
		i2s_set_clk(i2s_out_num, 44100, 16, 2);
	}
	fseek(f, 44, SEEK_SET);
	pos = 44;
	while(pos < size)
	{
		chunk = (size - pos) > CSIZE ? CSIZE : (size - pos);
		fread(narrate_data, 1, chunk, f);
		xRingbufferSend(audio_channel, (void *)narrate_data, chunk, (portTickType)portMAX_DELAY);
		pos += chunk;
	}
	if(s_rate != 44100){
		i2s_set_clk(i2s_out_num, s_rate, 16, 2);
	}
	fclose(f);

	vTaskDelay(1000 / portTICK_PERIOD_MS);
	STL_STATE = false;
}

void play_default(void* param)
{
	ESP_LOGI(TAG, "Executing: %s", __func__);
	FILE* f = fopen(MONOMAN, "r");
	if(f == NULL){
		MODE = NO_MODE;
		ESP_LOGE(TAG, "Problems");
		vTaskDelete(NULL);
	}
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f) - 60000;
    rewind(f);
    uint8_t buffer[CSIZE];
    size_t pos;
    uint32_t ins = STOP_DEF;
	uint32_t s_rate = i2s_get_clk(i2s_out_num);

	while(ins == STOP_DEF) xTaskNotifyWait(0, 0, &ins, portMAX_DELAY);
	if(s_rate != 44100){
		i2s_set_clk(i2s_out_num, 44100, 16, 2);
	}

    while(ins != ABORT){
    	fseek(f, 44, SEEK_SET);
    	pos = 44;
		i2s_zero_dma_buffer(i2s_out_num);
    	while((size - pos) > CSIZE)
		{
    		xTaskNotifyWait(0, 0, &ins, 0);
			if(ins == ABORT) break;
			else if(ins == STOP_DEF) {
				ESP_LOGI(TAG, "Default Stopped");
				while(ins == STOP_DEF) xTaskNotifyWait(0, 0, &ins, portMAX_DELAY);
				if(ins == START_DEF){
					ESP_LOGI(TAG, "Default Restarted");
					s_rate = i2s_get_clk(i2s_out_num);
					if(s_rate != 44100){
						i2s_set_clk(i2s_out_num, 44100, 16, 2);
					}
				}
			}
			while(STL_STATE) vTaskDelay(400 / portTICK_PERIOD_MS);
			fread(buffer, 1, CSIZE, f);
			write_ringbuf(buffer, CSIZE);
			pos += CSIZE;
		}
    }
    def_handle = NULL;
    fclose(f);
    ESP_LOGI(TAG, "Stopped %s", __func__);
    vTaskDelete(NULL);
}

void set_mode(uint16_t event, void *param){
	if(MODE == event){
		return;
	}
	esp_bd_addr_t cma;
	memcpy(cma, param, ESP_BD_ADDR_LEN);

	switch(event){
	case NO_MODE:
		if(MODE == DEFAULT_MODE){
			xTaskNotify(def_handle, STOP_DEF, eSetValueWithOverwrite);
		}
		else if(MODE == BLUETOOTH_MODE){
			esp_a2d_sink_disconnect(cma);
			vTaskDelay(1000 / portTICK_PERIOD_MS);
			esp_a2d_sink_deinit();
		}
		vTaskDelay(1000 / portTICK_PERIOD_MS);
		i2s_zero_dma_buffer(i2s_out_num);
		break;
	case DEFAULT_MODE:
		if(MODE == BLUETOOTH_MODE){
			esp_a2d_sink_disconnect(cma);
			vTaskDelay(1000 / portTICK_PERIOD_MS);
			esp_a2d_sink_deinit();
		}
		narrate(SWITCH_DEFAULT);
		xTaskNotify(def_handle, START_DEF, eSetValueWithOverwrite);
		break;
	case BLUETOOTH_MODE:
		if(MODE == DEFAULT_MODE){
			xTaskNotify(def_handle, STOP_DEF, eSetValueWithOverwrite);
		}
		esp_a2d_sink_init();
		vTaskDelay(1000 / portTICK_PERIOD_MS);
		narrate(SWITCH_BT);
		esp_a2d_sink_connect(cma);
		xTaskNotify(s_i2s_task_handle, (uint32_t)BT_VOL, eSetValueWithOverwrite);
		break;
	}
	MODE = event;
}

void change_volume(uint16_t event, void *param){
	if(event <= 10 && MODE == DEFAULT_MODE){
		xTaskNotify(s_i2s_task_handle, (uint32_t)event, eSetValueWithOverwrite);
	}
}

void indirect_narrate(uint16_t event, void *param){
	switch(event){
	case GM_NARRATE_EVENT:
		narrate(GOODMORNING);
		break;
	case GN_NARRATE_EVENT:
		narrate(GOODNIGHT);
		break;
	case BCS_NARRATE_EVENT:
		narrate(BATTERY_CRITICAL_SOUND);
		break;
	}
}

void cmd_active(uint16_t event, void *param){
	switch(event){
	case COMMAND_MODE_ACTIVE:
		OVL_STATE = true;
		narrate(CMD_YES_SOUND);
		break;
	case COMMAND_MODE_ACCEPTED:
		narrate(CMD_GOTIT_SOUND);
		OVL_STATE = false;
		break;
	case COMMAND_MODE_INACTIVE:
		narrate(CMD_ICGI_SOUND);
		OVL_STATE = false;
		break;
	}
}

void overlay_battery_status(uint16_t event, void *param){
	switch(event){
	case BATTERY_LOW:
		narrate(BATTERY_LOW_SOUND);
		break;
	case CRG_CONN:
		narrate(ADAPTER_CONN);
		break;
	case CRG_DISCONN:
		narrate(ADAPTER_DISC);
		break;
	}
}

void set_light(uint16_t event, void *param){
	if(LGT == event){
		return;
	}
	switch(event){
	case LIGHT_ON:
		xTaskNotify(color_handle, START_LGT, eSetValueWithOverwrite);
		break;
	case LIGHT_OFF:
		xTaskNotify(color_handle, STOP_LGT, eSetValueWithOverwrite);
		break;
	}
	LGT = event;
}

// ------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------

static void increment_color(uint8_t* C, bool* direction, uint8_t* index){
	if(*direction) C[*index] += 1;
	else C[*index] -= 1;
	if(C[*index] == 0xff || C[*index] == 0x00){
		*direction = !(*direction);
		*index = ((*index) + 2) % 3;
	}
}


void process_colors(void *param)
{
	ESP_LOGI(TAG, "Executing: %s", __func__);
#define DROP_RATE 0.005f
#define RISE_RATE 0.003f
#define THRESHOLD 0.7f
#define SMOOTHNESS 0.1f
#define HNL 5
#define HNR 4

	// --------------------------------------------------------------------------------------------

	uint8_t L_COLOR[3] = {0xff, 0x00, 0x00};
	uint8_t H_COLOR[3] = {0xff, 0x80, 0x00};
	bool L_SLOPE = true, H_SLOPE = true;
	uint8_t LCI = 1, HCI = 1;
	uint8_t INCR_WAIT = 0;

	// -----------------------------------------------------------------------------------------------
	// ================================
	uint8_t* buffer = (uint8_t*)param;
	// ================================
	uint8_t R, G, B;
	uint16_t i, j, k;
	int16_t left, right;
	float fft_table[CHUNK_SIZE] = {0};
	float flt_d[2*CHUNK_SIZE] = {0};
	float spectrum[HALF_CS] = {0};
	float CD[HN_LED];
	const uint16_t spi[87] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20, 22, 23, 25, 26, 28, 30, 31, 33, 35, 37, 39, 42, 44, 46, 49, 51, 54, 57, 60, 63, 66, 69, 73, 76, 80, 84, 88, 92, 96, 101, 105, 110, 115, 121, 126, 132, 138, 144, 151, 157, 164, 172, 179, 187, 195, 204, 213, 222, 232, 242, 252, 263, 275, 286, 299, 311, 325, 339, 353, 368, 384, 400, 417, 435, 453, 472, 511};
	const uint8_t spi_index[9][2] = {{0, 5}, {4, 10}, {9, 16}, {15, 24}, {23, 34}, {33, 45}, {44, 57}, {56, 71}, {70, 86}};

	float r, V;
	float rate[HN_LED] = {0};
	float MAX[HN_LED] = {0};
	float MIN[HN_LED] = {MAXFLOAT};
	float CS[HN_LED] = {0};
	float LD, RD, max_cd;
	uint32_t lgt = STOP_LGT;

	led_strip_t *strip = NULL;
	strip = led_strip_init(RMT_CHANNEL_0, WS2812B_DOUT, N_LED);
	if(strip == NULL){
		ESP_LOGE(TAG, "Problems with Strip");
		vTaskDelete(NULL);
	}
	//---------------------------------------------------------------------------------------------

	dsps_fft2r_init_fc32(fft_table, CHUNK_SIZE);
	dac_output_enable(NEON_1);
	dac_output_enable(NEON_2);

	strip->clear(strip, 500);
	while(lgt == STOP_LGT) xTaskNotifyWait(0, 0, &lgt, portMAX_DELAY);

	while(lgt != ABORT){
		if(OVL_STATE){
			for(i = 0; i < HN_LED; i++){
				strip->set_pixel(strip, i, H_COLOR[0], H_COLOR[1], H_COLOR[2]);
				strip->set_pixel(strip, i + HN_LED, H_COLOR[0], H_COLOR[1], H_COLOR[2]);
			}
			strip->refresh(strip, 100);
			dac_output_voltage(NEON_1, 255);
			dac_output_voltage(NEON_2, 255);
		}
		else{
			if(xSemaphoreTake(cdat_semaphore, 100 / portTICK_PERIOD_MS) == pdTRUE){
				for(i = 0; i < CHUNK_SIZE; i++){
					left = *((int16_t*)(buffer + 4*i));
					right = *((int16_t*)(buffer + 4*i + 2));
					flt_d[2*i] = ((float)(left + right)) / 2.0f;
					flt_d[2*i+1] = 0.0f;
				}
				if(dsps_fft2r_fc32_ae32_(flt_d, CHUNK_SIZE, fft_table) != ESP_OK){ continue; }
				for(i = 0; i < HALF_CS; i++){
					spectrum[i] = fabs(flt_d[2*i]) + fabs(flt_d[2*i + 1]);
				}

				for(i = 0; i < HN_LED; i++){
					CD[i] = 0.0f;
					for(k = spi_index[i][0]; k <= spi_index[i][1] - 2; k++){
						max_cd = 0.0f;
						for(j = spi[i] + 1; j < spi[i + 1]; j++) max_cd += spectrum[j] * (spi[i + 1] - j) / (spi[i + 1] - spi[i]);
						for(j = spi[i + 1]; j < spi[i + 2]; j++) max_cd += spectrum[j] * (spi[i + 2] - j) / (spi[i + 2] - spi[i + 1]);
						if(max_cd > CD[i]) CD[i] = max_cd;
					}
				}
			}
			else{
				for(i = 0; i < HN_LED; i++){ CD[i] = 0.0f; }
			}

			for(i = 0; i < HN_LED; i++){
				r = (CD[i] - CS[i]) * SMOOTHNESS;
				if(fabsf(r) > rate[i]){rate[i] = r;}
				CS[i] = CS[i] + rate[i];
				MAX[i] = CS[i] >= MAX[i] ? CS[i] : MAX[i] - DROP_RATE * (MAX[i] - CS[i]);
				MIN[i] = CS[i] <= MIN[i] ? CS[i] : MIN[i] + RISE_RATE * (CS[i] - MIN[i]);
			}

			LD = 0.0f; RD = 0.0f;
			for(i = 0; i < HN_LED; i++){
				V = MAX[i] == MIN[i] ? 0.0f : (CS[i] - MIN[i]) / (MAX[i] - MIN[i]);
				if(i<HNL) LD += V;
				else RD += V;

				if(V < THRESHOLD){
					R = floorf((float)L_COLOR[0] * V);
					G = floorf((float)L_COLOR[1] * V);
					B = floorf((float)L_COLOR[2] * V);
				}else{
					R = L_COLOR[0] + floorf((float)(H_COLOR[0] - L_COLOR[0]) * V);
					G = L_COLOR[1] + floorf((float)(H_COLOR[1] - L_COLOR[1]) * V);
					B = L_COLOR[2] + floorf((float)(H_COLOR[2] - L_COLOR[2]) * V);
				}
				strip->set_pixel(strip, i, R, G, B);
				strip->set_pixel(strip, i + HN_LED, R, G, B);
			}
			strip->refresh(strip, 100);
			dac_output_voltage(NEON_1, 35 + floorf((LD / HNL) * 220));
			dac_output_voltage(NEON_2, 35 + floorf((RD / HNR) * 220));
		}

		if(INCR_WAIT < 10) INCR_WAIT += 1;
		else {
			INCR_WAIT = 0;
			increment_color(H_COLOR, &H_SLOPE, &HCI);
			increment_color(L_COLOR, &L_SLOPE, &LCI);
		}

		if(xTaskNotifyWait(0, 0xffffffff, &lgt, 0) == pdTRUE){
			if(lgt == STOP_LGT){
				strip->clear(strip, 500);
				dac_output_voltage(NEON_1, 0);
				while(lgt == STOP_LGT) xTaskNotifyWait(0, 0, &lgt, portMAX_DELAY);
			}
		}
	}
	strip->clear(strip, 500);
	dac_output_voltage(NEON_1, 0);
	dac_output_voltage(NEON_2, 0);
	led_strip_denit(strip);

	color_handle = NULL;
	ESP_LOGI(TAG, "Stopped %s", __func__);
	vTaskDelete(NULL);
}

