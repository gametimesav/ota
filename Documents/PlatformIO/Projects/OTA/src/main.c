#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifndef FIRMWARE_URL
#define FIRMWARE_URL "https://github.com/gametimesav/ota/releases/latest/download/firmware.bin"
#endif

#ifndef DISPLAY_MESSAGE
#define DISPLAY_MESSAGE "OTA v0.2.5 - update path demo"
#endif

#define WIFI_CONNECTED_BIT BIT0
#define MAX_SCAN_APS 20
#define CRED_NAMESPACE "wifi_cfg"
#define AP_SSID "ESP32-OTA-Setup"
#define AP_PASS ""

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 240
#define LCD_V_RES 320
#define LCD_PIXEL_CLOCK_HZ (26 * 1000 * 1000)

#define LCD_PIN_NUM_MOSI 13
#define LCD_PIN_NUM_CLK 14
#define LCD_PIN_NUM_CS 15
#define LCD_PIN_NUM_DC 2
#define LCD_PIN_NUM_RST -1
#define LCD_PIN_NUM_BCKL 21

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_RED 0xF800
#define COLOR_GREEN 0x07E0
#define COLOR_BLUE 0x001F
#define COLOR_YELLOW 0xFFE0
#define COLOR_CYAN 0x07FF

#define STATUS_BAR_Y 4
#define STATUS_BAR_H 14
#define STATUS_AREA_Y 170
#define STATUS_AREA_H (LCD_V_RES - STATUS_AREA_Y)

static const char *TAG = "ota_example";
static EventGroupHandle_t wifi_event_group;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static httpd_handle_t portal_server;
static esp_lcd_panel_handle_t lcd_panel;
static bool display_ready;
static SemaphoreHandle_t display_mutex;

typedef struct {
	char ssid[33];
	char pass[65];
} wifi_creds_t;

typedef enum {
	OTA_RESULT_SKIPPED = 0,
	OTA_RESULT_UPDATED,
	OTA_RESULT_FAILED,
} ota_result_t;

typedef struct {
	int major;
	int minor;
	int patch;
} semver_t;

static bool parse_semver(const char *version, semver_t *out)
{
	if (version == NULL || out == NULL) {
		return false;
	}

	semver_t tmp = {0};
	if (sscanf(version, "%d.%d.%d", &tmp.major, &tmp.minor, &tmp.patch) == 3) {
		*out = tmp;
		return true;
	}

	return false;
}

static int semver_compare(const char *current, const char *remote)
{
	semver_t cur = {0};
	semver_t rem = {0};

	if (!parse_semver(current, &cur) || !parse_semver(remote, &rem)) {
		return strcmp(remote, current) != 0 ? 1 : 0;
	}

	if (rem.major != cur.major) {
		return rem.major > cur.major ? 1 : -1;
	}

	if (rem.minor != cur.minor) {
		return rem.minor > cur.minor ? 1 : -1;
	}

	if (rem.patch != cur.patch) {
		return rem.patch > cur.patch ? 1 : -1;
	}

	return 0;
}

static bool glyph_5x7(char c, uint8_t rows[7])
{
	memset(rows, 0, 7);
	switch (c) {
	case 'A':
		rows[0] = 0x0E;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x1F;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x11;
		return true;
	case 'B':
		rows[0] = 0x1E;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x1E;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x1E;
		return true;
	case 'C':
		rows[0] = 0x0E;
		rows[1] = 0x11;
		rows[2] = 0x10;
		rows[3] = 0x10;
		rows[4] = 0x10;
		rows[5] = 0x11;
		rows[6] = 0x0E;
		return true;
	case 'D':
		rows[0] = 0x1E;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x11;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x1E;
		return true;
	case 'H':
		rows[0] = 0x11;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x1F;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x11;
		return true;
	case 'E':
		rows[0] = 0x1F;
		rows[1] = 0x10;
		rows[2] = 0x10;
		rows[3] = 0x1E;
		rows[4] = 0x10;
		rows[5] = 0x10;
		rows[6] = 0x1F;
		return true;
	case 'F':
		rows[0] = 0x1F;
		rows[1] = 0x10;
		rows[2] = 0x10;
		rows[3] = 0x1E;
		rows[4] = 0x10;
		rows[5] = 0x10;
		rows[6] = 0x10;
		return true;
	case 'G':
		rows[0] = 0x0E;
		rows[1] = 0x11;
		rows[2] = 0x10;
		rows[3] = 0x17;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x0E;
		return true;
	case 'I':
		rows[0] = 0x1F;
		rows[1] = 0x04;
		rows[2] = 0x04;
		rows[3] = 0x04;
		rows[4] = 0x04;
		rows[5] = 0x04;
		rows[6] = 0x1F;
		return true;
	case 'K':
		rows[0] = 0x11;
		rows[1] = 0x12;
		rows[2] = 0x14;
		rows[3] = 0x18;
		rows[4] = 0x14;
		rows[5] = 0x12;
		rows[6] = 0x11;
		return true;
	case 'L':
		rows[0] = 0x10;
		rows[1] = 0x10;
		rows[2] = 0x10;
		rows[3] = 0x10;
		rows[4] = 0x10;
		rows[5] = 0x10;
		rows[6] = 0x1F;
		return true;
	case 'M':
		rows[0] = 0x11;
		rows[1] = 0x1B;
		rows[2] = 0x15;
		rows[3] = 0x15;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x11;
		return true;
	case 'N':
		rows[0] = 0x11;
		rows[1] = 0x19;
		rows[2] = 0x15;
		rows[3] = 0x13;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x11;
		return true;
	case 'O':
		rows[0] = 0x0E;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x11;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x0E;
		return true;
	case 'W':
		rows[0] = 0x11;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x15;
		rows[4] = 0x15;
		rows[5] = 0x15;
		rows[6] = 0x0A;
		return true;
	case 'P':
		rows[0] = 0x1E;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x1E;
		rows[4] = 0x10;
		rows[5] = 0x10;
		rows[6] = 0x10;
		return true;
	case 'R':
		rows[0] = 0x1E;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x1E;
		rows[4] = 0x14;
		rows[5] = 0x12;
		rows[6] = 0x11;
		return true;
	case 'S':
		rows[0] = 0x0F;
		rows[1] = 0x10;
		rows[2] = 0x10;
		rows[3] = 0x0E;
		rows[4] = 0x01;
		rows[5] = 0x01;
		rows[6] = 0x1E;
		return true;
	case 'T':
		rows[0] = 0x1F;
		rows[1] = 0x04;
		rows[2] = 0x04;
		rows[3] = 0x04;
		rows[4] = 0x04;
		rows[5] = 0x04;
		rows[6] = 0x04;
		return true;
	case 'U':
		rows[0] = 0x11;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x11;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x0E;
		return true;
	case 'V':
		rows[0] = 0x11;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x11;
		rows[4] = 0x11;
		rows[5] = 0x0A;
		rows[6] = 0x04;
		return true;
	case 'Y':
		rows[0] = 0x11;
		rows[1] = 0x11;
		rows[2] = 0x0A;
		rows[3] = 0x04;
		rows[4] = 0x04;
		rows[5] = 0x04;
		rows[6] = 0x04;
		return true;
	case '0':
		rows[0] = 0x0E;
		rows[1] = 0x11;
		rows[2] = 0x13;
		rows[3] = 0x15;
		rows[4] = 0x19;
		rows[5] = 0x11;
		rows[6] = 0x0E;
		return true;
	case '1':
		rows[0] = 0x04;
		rows[1] = 0x0C;
		rows[2] = 0x04;
		rows[3] = 0x04;
		rows[4] = 0x04;
		rows[5] = 0x04;
		rows[6] = 0x1F;
		return true;
	case '2':
		rows[0] = 0x0E;
		rows[1] = 0x11;
		rows[2] = 0x01;
		rows[3] = 0x02;
		rows[4] = 0x04;
		rows[5] = 0x08;
		rows[6] = 0x1F;
		return true;
	case '3':
		rows[0] = 0x1E;
		rows[1] = 0x01;
		rows[2] = 0x01;
		rows[3] = 0x0E;
		rows[4] = 0x01;
		rows[5] = 0x01;
		rows[6] = 0x1E;
		return true;
	case '4':
		rows[0] = 0x02;
		rows[1] = 0x06;
		rows[2] = 0x0A;
		rows[3] = 0x12;
		rows[4] = 0x1F;
		rows[5] = 0x02;
		rows[6] = 0x02;
		return true;
	case '5':
		rows[0] = 0x1F;
		rows[1] = 0x10;
		rows[2] = 0x10;
		rows[3] = 0x1E;
		rows[4] = 0x01;
		rows[5] = 0x01;
		rows[6] = 0x1E;
		return true;
	case '6':
		rows[0] = 0x0E;
		rows[1] = 0x10;
		rows[2] = 0x10;
		rows[3] = 0x1E;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x0E;
		return true;
	case '7':
		rows[0] = 0x1F;
		rows[1] = 0x01;
		rows[2] = 0x02;
		rows[3] = 0x04;
		rows[4] = 0x08;
		rows[5] = 0x08;
		rows[6] = 0x08;
		return true;
	case '8':
		rows[0] = 0x0E;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x0E;
		rows[4] = 0x11;
		rows[5] = 0x11;
		rows[6] = 0x0E;
		return true;
	case '9':
		rows[0] = 0x0E;
		rows[1] = 0x11;
		rows[2] = 0x11;
		rows[3] = 0x0F;
		rows[4] = 0x01;
		rows[5] = 0x01;
		rows[6] = 0x0E;
		return true;
	case '.':
		rows[6] = 0x04;
		return true;
	case ' ':
		return true;
	default:
		return false;
	}
}

static void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
	if (!display_ready || lcd_panel == NULL) {
		return;
	}
	if (w <= 0 || h <= 0) {
		return;
	}
	if (x < 0 || y < 0 || x + w > LCD_H_RES || y + h > LCD_V_RES) {
		return;
	}

	uint16_t line[w];
	for (int i = 0; i < w; i++) {
		line[i] = color;
	}

	for (int row = 0; row < h; row++) {
		esp_lcd_panel_draw_bitmap(lcd_panel, x, y + row, x + w, y + row + 1, line);
	}
}

static void display_clear(uint16_t color)
{
	display_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, color);
}

static void display_draw_char_5x7(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
	if (!display_ready || lcd_panel == NULL || scale < 1) {
		return;
	}

	uint8_t glyph[7];
	if (!glyph_5x7(c, glyph)) {
		return;
	}

	for (int row = 0; row < 7; row++) {
		for (int sy = 0; sy < scale; sy++) {
			uint16_t line[5 * scale];
			for (int col = 0; col < 5; col++) {
				bool on = (glyph[row] & (1 << (4 - col))) != 0;
				for (int sx = 0; sx < scale; sx++) {
					line[col * scale + sx] = on ? fg : bg;
				}
			}

			esp_lcd_panel_draw_bitmap(lcd_panel,
								 x,
								 y + (row * scale) + sy,
								 x + (5 * scale),
								 y + (row * scale) + sy + 1,
								 line);
		}
	}
}

static void display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
	int cursor = x;
	for (size_t i = 0; text[i] != '\0'; i++) {
		display_draw_char_5x7(cursor, y, text[i], fg, bg, scale);
		cursor += 6 * scale;
	}
}

static void display_draw_text_centered(int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
	if (text == NULL) {
		return;
	}

	int width = (int)strlen(text) * 6 * scale;
	int x = (LCD_H_RES - width) / 2;
	if (x < 0) {
		x = 0;
	}
	display_draw_text(x, y, text, fg, bg, scale);
}

static bool display_lock(void)
{
	if (display_mutex == NULL) {
		return false;
	}
	return xSemaphoreTake(display_mutex, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void display_unlock(void)
{
	if (display_mutex != NULL) {
		xSemaphoreGive(display_mutex);
	}
}

static void display_show_status_locked(const char *line1, const char *line2, uint16_t status_color)
{
	if (!display_ready) {
		return;
	}

	display_fill_rect(0, STATUS_BAR_Y, LCD_H_RES, STATUS_BAR_H, status_color);
	display_fill_rect(0, STATUS_AREA_Y, LCD_H_RES, STATUS_AREA_H, COLOR_BLACK);

	if (line1 != NULL && line1[0] != '\0') {
		display_draw_text_centered(182, line1, COLOR_CYAN, COLOR_BLACK, 2);
	}
	if (line2 != NULL && line2[0] != '\0') {
		display_draw_text_centered(205, line2, COLOR_WHITE, COLOR_BLACK, 2);
	}
}

static void display_show_status(const char *line1, const char *line2, uint16_t status_color)
{
	if (!display_lock()) {
		return;
	}
	display_show_status_locked(line1, line2, status_color);
	display_unlock();
}

static void display_show_boot_splash(const char *version)
{
	display_clear(COLOR_BLACK);
	display_draw_text_centered(120, "HELLO WORLD", COLOR_WHITE, COLOR_BLACK, 3);

	char version_line[32];
	snprintf(version_line, sizeof(version_line), "VER %.26s", version);
	display_draw_text_centered(190, version_line, COLOR_YELLOW, COLOR_BLACK, 2);
	display_show_status_locked("BOOT", "STARTING", COLOR_BLUE);
}

static esp_err_t display_init(void)
{
	if (display_ready) {
		return ESP_OK;
	}

	spi_bus_config_t bus_config = {
		.sclk_io_num = LCD_PIN_NUM_CLK,
		.mosi_io_num = LCD_PIN_NUM_MOSI,
		.miso_io_num = -1,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = LCD_H_RES * 32 * sizeof(uint16_t),
	};

	esp_err_t err = spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO);
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		return err;
	}

	esp_lcd_panel_io_handle_t io_handle = NULL;
	esp_lcd_panel_io_spi_config_t io_config = {
		.dc_gpio_num = LCD_PIN_NUM_DC,
		.cs_gpio_num = LCD_PIN_NUM_CS,
		.pclk_hz = LCD_PIXEL_CLOCK_HZ,
		.lcd_cmd_bits = 8,
		.lcd_param_bits = 8,
		.spi_mode = 0,
		.trans_queue_depth = 10,
	};

	err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle);
	if (err != ESP_OK) {
		return err;
	}

	esp_lcd_panel_dev_config_t panel_config = {
		.reset_gpio_num = LCD_PIN_NUM_RST,
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = 16,
	};

	err = esp_lcd_new_panel_st7789(io_handle, &panel_config, &lcd_panel);
	if (err != ESP_OK) {
		return err;
	}

	ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
	ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
	ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd_panel, true));
	ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, true, false));
	ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_panel, true));
	ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));

	if (LCD_PIN_NUM_BCKL >= 0) {
		gpio_config_t backlight_config = {
			.mode = GPIO_MODE_OUTPUT,
			.pin_bit_mask = 1ULL << LCD_PIN_NUM_BCKL,
		};
		ESP_ERROR_CHECK(gpio_config(&backlight_config));
		ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_NUM_BCKL, 1));
	}

	if (display_mutex == NULL) {
		display_mutex = xSemaphoreCreateMutex();
		if (display_mutex == NULL) {
			return ESP_ERR_NO_MEM;
		}
	}

	display_ready = true;
	return ESP_OK;
}

static void display_show_message(const char *message)
{
	ESP_LOGI(TAG, "Display message requested: %s", message);

	esp_err_t err = display_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(err));
		return;
	}

	if (!display_lock()) {
		ESP_LOGE(TAG, "Display lock failed");
		return;
	}
	const esp_app_desc_t *app = esp_app_get_description();
	display_show_boot_splash(app->version);
	display_unlock();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
		ESP_LOGW(TAG, "Wi-Fi disconnected");
		display_show_status("WIFI", "DISCONNECTED", COLOR_RED);
		return;
	}

	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
		ESP_LOGI(TAG, "Wi-Fi connected");
		display_show_status("WIFI", "CONNECTED", COLOR_GREEN);
	}
}

static void restart_task(void *arg)
{
	vTaskDelay(pdMS_TO_TICKS(1200));
	esp_restart();
}

static void url_decode(char *s)
{
	char *src = s;
	char *dst = s;

	while (*src != '\0') {
		if (*src == '+') {
			*dst++ = ' ';
			src++;
			continue;
		}

		if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
			char hex[3] = {src[1], src[2], '\0'};
			*dst++ = (char)strtol(hex, NULL, 16);
			src += 3;
			continue;
		}

		*dst++ = *src++;
	}

	*dst = '\0';
}

static bool creds_load(wifi_creds_t *creds)
{
	nvs_handle_t handle;
	size_t ssid_len = sizeof(creds->ssid);
	size_t pass_len = sizeof(creds->pass);

	memset(creds, 0, sizeof(*creds));
	if (nvs_open(CRED_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
		return false;
	}

	if (nvs_get_str(handle, "ssid", creds->ssid, &ssid_len) != ESP_OK ||
		nvs_get_str(handle, "pass", creds->pass, &pass_len) != ESP_OK) {
		nvs_close(handle);
		return false;
	}

	nvs_close(handle);
	return strlen(creds->ssid) > 0;
}

static esp_err_t creds_save(const wifi_creds_t *creds)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(CRED_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		return err;
	}

	err = nvs_set_str(handle, "ssid", creds->ssid);
	if (err == ESP_OK) {
		err = nvs_set_str(handle, "pass", creds->pass);
	}
	if (err == ESP_OK) {
		err = nvs_commit(handle);
	}

	nvs_close(handle);
	return err;
}

static void wifi_stack_init(void)
{
	wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	sta_netif = esp_netif_create_default_wifi_sta();
	ap_netif = esp_netif_create_default_wifi_ap();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
										ESP_EVENT_ANY_ID,
										&wifi_event_handler,
										NULL,
										NULL));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
										IP_EVENT_STA_GOT_IP,
										&wifi_event_handler,
										NULL,
										NULL));
}

static bool wifi_try_connect(const wifi_creds_t *creds, int timeout_ms)
{
	wifi_config_t sta_cfg = {0};
	esp_err_t stop_err;
	display_show_status("WIFI", "CONNECTING", COLOR_BLUE);

	strncpy((char *)sta_cfg.sta.ssid, creds->ssid, sizeof(sta_cfg.sta.ssid) - 1);
	strncpy((char *)sta_cfg.sta.password, creds->pass, sizeof(sta_cfg.sta.password) - 1);
	sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

	stop_err = esp_wifi_stop();
	if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_INIT && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
		ESP_ERROR_CHECK(stop_err);
	}
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_connect());

	EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
									  WIFI_CONNECTED_BIT,
									  pdFALSE,
									  pdFALSE,
									  pdMS_TO_TICKS(timeout_ms));

	if ((bits & WIFI_CONNECTED_BIT) != 0) {
		ESP_LOGI(TAG, "Connected to SSID: %s", creds->ssid);
		display_show_status("WIFI", "CONNECTED", COLOR_GREEN);
		return true;
	}

	ESP_LOGW(TAG, "Wi-Fi connect timeout for SSID: %s", creds->ssid);
	display_show_status("WIFI", "CONNECT FAIL", COLOR_RED);
	return false;
}

static char *build_ssid_options_html(void)
{
	uint16_t ap_count = 0;
	wifi_ap_record_t ap_records[MAX_SCAN_APS];
	char *html = malloc(2048);
	if (html == NULL) {
		return NULL;
	}

	html[0] = '\0';
	ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
	if (ap_count > MAX_SCAN_APS) {
		ap_count = MAX_SCAN_APS;
	}
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));

	for (uint16_t i = 0; i < ap_count; i++) {
		const char *ssid = (const char *)ap_records[i].ssid;
		if (ssid[0] == '\0') {
			continue;
		}

		char line[160];
		snprintf(line,
				 sizeof(line),
				 "<option value=\"%s\">%s (%d dBm)</option>",
				 ssid,
				 ssid,
				 ap_records[i].rssi);
		strncat(html, line, 2047 - strlen(html));
	}

	if (html[0] == '\0') {
		strncpy(html, "<option value=\"\">No networks found</option>", 2047);
	}

	return html;
}

static esp_err_t portal_root_get_handler(httpd_req_t *req)
{
	char *options = build_ssid_options_html();
	if (options == NULL) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}

	const char *html_head =
		"<!doctype html><html><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
		"<title>ESP32 Wi-Fi Setup</title>"
		"<style>body{font-family:sans-serif;background:#f6f7fb;padding:20px;}"
		".card{max-width:420px;margin:auto;background:#fff;border-radius:14px;padding:18px;box-shadow:0 10px 30px rgba(0,0,0,.08);}"
		"select,input,button{width:100%;padding:10px;margin-top:10px;border-radius:8px;border:1px solid #ccd1dd;}"
		"button{background:#0f766e;color:#fff;border:0;font-weight:600;}</style></head><body><div class=\"card\">"
		"<h2>Configure Wi-Fi</h2><p>Select network and enter password.</p>"
		"<form method=\"POST\" action=\"/save\"><label>SSID</label><select name=\"ssid\">";

	const char *html_tail =
		"</select><label>Password</label><input type=\"password\" name=\"pass\" required>"
		"<button type=\"submit\">Save and Connect</button></form>"
		"<form method=\"GET\" action=\"/\"><button type=\"submit\">Rescan Networks</button></form>"
		"</div></body></html>";

	httpd_resp_set_type(req, "text/html");
	httpd_resp_sendstr_chunk(req, html_head);
	httpd_resp_sendstr_chunk(req, options);
	httpd_resp_sendstr_chunk(req, html_tail);
	httpd_resp_sendstr_chunk(req, NULL);

	free(options);
	return ESP_OK;
}

static esp_err_t portal_save_post_handler(httpd_req_t *req)
{
	char body[256];
	int read = httpd_req_recv(req, body, sizeof(body) - 1);
	if (read <= 0) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}

	body[read] = '\0';
	char *ssid = strstr(body, "ssid=");
	char *pass = strstr(body, "pass=");
	if (ssid == NULL || pass == NULL) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}

	ssid += 5;
	char *ssid_end = strchr(ssid, '&');
	if (ssid_end != NULL) {
		*ssid_end = '\0';
	}
	pass += 5;

	url_decode(ssid);
	url_decode(pass);

	wifi_creds_t creds = {0};
	strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
	strncpy(creds.pass, pass, sizeof(creds.pass) - 1);

	if (strlen(creds.ssid) == 0 || strlen(creds.pass) == 0) {
		httpd_resp_sendstr(req, "SSID and password are required.");
		return ESP_OK;
	}

	if (creds_save(&creds) != ESP_OK) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}

	httpd_resp_sendstr(req, "Saved. Device will restart and connect to Wi-Fi.");
	xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
	return ESP_OK;
}

static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", "/");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

static void captive_portal_start(void)
{
	wifi_config_t ap_cfg = {
		.ap = {
			.max_connection = 4,
			.authmode = WIFI_AUTH_OPEN,
			.channel = 1,
		},
	};

	strncpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
	ap_cfg.ap.ssid_len = strlen(AP_SSID);
	if (strlen(AP_PASS) > 0) {
		strncpy((char *)ap_cfg.ap.password, AP_PASS, sizeof(ap_cfg.ap.password) - 1);
		ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
	}

	esp_err_t stop_err = esp_wifi_stop();
	if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_INIT && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
		ESP_ERROR_CHECK(stop_err);
	}
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
	ESP_ERROR_CHECK(esp_wifi_start());

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.uri_match_fn = httpd_uri_match_wildcard;
	ESP_ERROR_CHECK(httpd_start(&portal_server, &config));

	httpd_uri_t root = {
		.uri = "/",
		.method = HTTP_GET,
		.handler = portal_root_get_handler,
	};
	httpd_uri_t save = {
		.uri = "/save",
		.method = HTTP_POST,
		.handler = portal_save_post_handler,
	};
	httpd_uri_t catch_all = {
		.uri = "/*",
		.method = HTTP_GET,
		.handler = portal_redirect_handler,
	};

	ESP_ERROR_CHECK(httpd_register_uri_handler(portal_server, &root));
	ESP_ERROR_CHECK(httpd_register_uri_handler(portal_server, &save));
	ESP_ERROR_CHECK(httpd_register_uri_handler(portal_server, &catch_all));

	ESP_LOGI(TAG, "Captive portal started on AP '%s' at http://192.168.4.1", AP_SSID);
	display_show_status("WIFI", "PORTAL MODE", COLOR_YELLOW);
}

static ota_result_t ota_update_from_github(void)
{
	esp_http_client_config_t http_config = {
		.url = FIRMWARE_URL,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.timeout_ms = 15000,
		.buffer_size = 8192,
		.buffer_size_tx = 2048,
		.keep_alive_enable = true,
	};

	esp_https_ota_config_t ota_config = {
		.http_config = &http_config,
	};
	esp_https_ota_handle_t ota_handle = NULL;
	const esp_app_desc_t *running_app = esp_app_get_description();
	esp_app_desc_t remote_app;

	ESP_LOGI(TAG, "Starting OTA from: %s", FIRMWARE_URL);
	display_show_status("OTA", "CHECKING", COLOR_BLUE);
	esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
		display_show_status("OTA", "START FAIL", COLOR_RED);
		return OTA_RESULT_FAILED;
	}

	err = esp_https_ota_get_img_desc(ota_handle, &remote_app);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to read remote firmware descriptor: %s", esp_err_to_name(err));
		esp_https_ota_abort(ota_handle);
		display_show_status("OTA", "DESC FAIL", COLOR_RED);
		return OTA_RESULT_FAILED;
	}

	ESP_LOGI(TAG, "Current firmware version: %s", running_app->version);
	ESP_LOGI(TAG, "Remote firmware version:  %s", remote_app.version);
	if (semver_compare(running_app->version, remote_app.version) <= 0) {
		ESP_LOGI(TAG, "OTA final state: skipped (already up-to-date)");
		esp_https_ota_abort(ota_handle);
		display_show_status("OTA", "UP TO DATE", COLOR_GREEN);
		return OTA_RESULT_SKIPPED;
	}

	display_show_status("OTA", "UPDATING", COLOR_YELLOW);

	do {
		err = esp_https_ota_perform(ota_handle);
		if (err != ESP_OK && err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
			ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
			esp_https_ota_abort(ota_handle);
			display_show_status("OTA", "WRITE FAIL", COLOR_RED);
			return OTA_RESULT_FAILED;
		}
	} while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

	if (!esp_https_ota_is_complete_data_received(ota_handle)) {
		ESP_LOGE(TAG, "Incomplete OTA image received");
		esp_https_ota_abort(ota_handle);
		display_show_status("OTA", "INCOMPLETE", COLOR_RED);
		return OTA_RESULT_FAILED;
	}

	err = esp_https_ota_finish(ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
		display_show_status("OTA", "FINISH FAIL", COLOR_RED);
		return OTA_RESULT_FAILED;
	}

	ESP_LOGI(TAG, "OTA final state: updated (restarting now)");
	display_show_status("OTA", "UPDATED", COLOR_GREEN);
	vTaskDelay(pdMS_TO_TICKS(600));
	esp_restart();
	return OTA_RESULT_UPDATED;
}

static void ota_task(void *arg)
{
	display_show_status("OTA", "TASK START", COLOR_BLUE);
	ota_result_t result = ota_update_from_github();
	if (result == OTA_RESULT_FAILED) {
		ESP_LOGE(TAG, "OTA final state: failed");
		display_show_status("OTA", "FAILED", COLOR_RED);
	} else if (result == OTA_RESULT_SKIPPED) {
		ESP_LOGI(TAG, "OTA final state: skipped");
		display_show_status("OTA", "SKIPPED", COLOR_GREEN);
	}
	vTaskDelete(NULL);
}

void app_main(void)
{
	wifi_creds_t creds = {0};
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	display_show_message(DISPLAY_MESSAGE);
	wifi_stack_init();

	if (creds_load(&creds) && wifi_try_connect(&creds, 15000)) {
		xTaskCreate(ota_task, "ota_task", 12288, NULL, 5, NULL);
	} else {
		captive_portal_start();
	}

	while (true) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}