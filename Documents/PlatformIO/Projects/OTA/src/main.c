#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif

#ifndef FIRMWARE_URL
#define FIRMWARE_URL "https://github.com/gametimesav/ota/releases/latest/download/firmware.bin"
#endif

#ifndef DISPLAY_MESSAGE
#define DISPLAY_MESSAGE "OTA test firmware v0.2 - screen updated"
#endif

static const char *TAG = "ota_example";
static EventGroupHandle_t wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0

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
		/* Fallback: if not semver, only update when strings differ */
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

static void display_show_message(const char *message)
{
	/* Replace with TFT/LVGL draw call when panel driver is wired in this project. */
	ESP_LOGI(TAG, "Display screen: %s", message);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
		return;
	}

	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
		esp_wifi_connect();
		xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
		return;
	}

	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
		ESP_LOGI(TAG, "Wi-Fi connected");
	}
}

static esp_err_t wifi_init_sta(void)
{
	wifi_event_group = xEventGroupCreate();
	if (wifi_event_group == NULL) {
		return ESP_ERR_NO_MEM;
	}

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

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

	wifi_config_t wifi_config = {
		.sta = {
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,
		},
	};

	strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
	strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "Connecting to SSID: %s", WIFI_SSID);

	xEventGroupWaitBits(wifi_event_group,
						WIFI_CONNECTED_BIT,
						pdFALSE,
						pdFALSE,
						portMAX_DELAY);

	return ESP_OK;
}

static ota_result_t ota_update_from_github(void)
{
	esp_http_client_config_t http_config = {
		.url = FIRMWARE_URL,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.timeout_ms = 15000,
		.keep_alive_enable = true,
	};

	esp_https_ota_config_t ota_config = {
		.http_config = &http_config,
	};
	esp_https_ota_handle_t ota_handle = NULL;
	const esp_app_desc_t *running_app = esp_app_get_description();
	esp_app_desc_t remote_app;

	ESP_LOGI(TAG, "Starting OTA from: %s", FIRMWARE_URL);
	esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
		return OTA_RESULT_FAILED;
	}

	err = esp_https_ota_get_img_desc(ota_handle, &remote_app);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to read remote firmware descriptor: %s", esp_err_to_name(err));
		esp_https_ota_abort(ota_handle);
		return OTA_RESULT_FAILED;
	}

	ESP_LOGI(TAG, "Current firmware version: %s", running_app->version);
	ESP_LOGI(TAG, "Remote firmware version:  %s", remote_app.version);
	int version_cmp = semver_compare(running_app->version, remote_app.version);

	if (version_cmp >= 0) {
		ESP_LOGI(TAG, "OTA status: up-to-date (%s)", running_app->version);
		ESP_LOGI(TAG, "No newer firmware available, OTA skipped");
		esp_https_ota_abort(ota_handle);
		return OTA_RESULT_SKIPPED;
	}

	ESP_LOGI(TAG, "OTA status: update available (%s -> %s)",
			 running_app->version,
			 remote_app.version);

	do {
		err = esp_https_ota_perform(ota_handle);
		if (err != ESP_OK && err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
			ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
			esp_https_ota_abort(ota_handle);
			return OTA_RESULT_FAILED;
		}
	} while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

	if (!esp_https_ota_is_complete_data_received(ota_handle)) {
		ESP_LOGE(TAG, "Incomplete OTA image received");
		esp_https_ota_abort(ota_handle);
		return OTA_RESULT_FAILED;
	}

	err = esp_https_ota_finish(ota_handle);
	if (err == ESP_OK) {
		ESP_LOGI(TAG, "OTA successful, restarting...");
		ESP_LOGI(TAG, "OTA final state: updated (restarting now)");
		esp_restart();
		return OTA_RESULT_UPDATED;
	}

	ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
	return OTA_RESULT_FAILED;
}

void app_main(void)
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	display_show_message(DISPLAY_MESSAGE);
	ESP_ERROR_CHECK(wifi_init_sta());
	ota_result_t ota_result = ota_update_from_github();
	if (ota_result == OTA_RESULT_SKIPPED) {
		ESP_LOGI(TAG, "OTA final state: skipped (already up-to-date)");
	} else if (ota_result == OTA_RESULT_FAILED) {
		ESP_LOGE(TAG, "OTA final state: failed");
	}
}