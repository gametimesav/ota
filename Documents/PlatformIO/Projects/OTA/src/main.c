#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifndef FIRMWARE_URL
#define FIRMWARE_URL "https://github.com/gametimesav/ota/releases/latest/download/firmware.bin"
#endif

#ifndef DISPLAY_MESSAGE
#define DISPLAY_MESSAGE "OTA test firmware v0.2.1 - small display tweak"
#endif

#define WIFI_CONNECTED_BIT BIT0
#define MAX_SCAN_APS 20
#define CRED_NAMESPACE "wifi_cfg"
#define AP_SSID "ESP32-OTA-Setup"
#define AP_PASS ""

static const char *TAG = "ota_example";
static EventGroupHandle_t wifi_event_group;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static httpd_handle_t portal_server;

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

static void display_show_message(const char *message)
{
	ESP_LOGI(TAG, "Display screen: %s", message);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
		ESP_LOGW(TAG, "Wi-Fi disconnected");
		return;
	}

	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
		ESP_LOGI(TAG, "Wi-Fi connected");
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
		return true;
	}

	ESP_LOGW(TAG, "Wi-Fi connect timeout for SSID: %s", creds->ssid);
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
	if (semver_compare(running_app->version, remote_app.version) >= 0) {
		ESP_LOGI(TAG, "OTA final state: skipped (already up-to-date)");
		esp_https_ota_abort(ota_handle);
		return OTA_RESULT_SKIPPED;
	}

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
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
		return OTA_RESULT_FAILED;
	}

	ESP_LOGI(TAG, "OTA final state: updated (restarting now)");
	esp_restart();
	return OTA_RESULT_UPDATED;
}

static void ota_task(void *arg)
{
	ota_result_t result = ota_update_from_github();
	if (result == OTA_RESULT_FAILED) {
		ESP_LOGE(TAG, "OTA final state: failed");
	} else if (result == OTA_RESULT_SKIPPED) {
		ESP_LOGI(TAG, "OTA final state: skipped");
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