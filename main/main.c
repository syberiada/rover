#include "esp_camera.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static const char *TAG = "camera_server";

// Pin mapping for Seeed Studio XIAO ESP32S3 Sense (OV2640)
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 10
#define SIOD_GPIO_NUM 40
#define SIOC_GPIO_NUM 39

#define Y9_GPIO_NUM 48
#define Y8_GPIO_NUM 11
#define Y7_GPIO_NUM 12
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 16
#define Y4_GPIO_NUM 18
#define Y3_GPIO_NUM 17
#define Y2_GPIO_NUM 15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM 47
#define PCLK_GPIO_NUM 13

// HTTP response headers
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=frame"
#define STREAM_BOUNDARY "\r\n--frame\r\n"
#define STREAM_PART "Content-Type:image/jpeg\r\nContent-Length: %u\r\n\r\n"

static esp_err_t jpg_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    ESP_LOGE(TAG, "Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  char part_buf[64];
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      ESP_LOGE(TAG, "Camera capture failed");
      return ESP_FAIL;
    }

    int hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
    if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) !=
            ESP_OK ||
        httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
        httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) != ESP_OK) {
      esp_camera_fb_return(fb);
      return ESP_FAIL;
    }
    esp_camera_fb_return(fb);
    vTaskDelay(1);
  }
  return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *req) {
  const char *html = "ESP32S3 Camera";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static httpd_handle_t start_webserver(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t server = NULL;

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_uri_t uri_root = {.uri = "/",
                            .method = HTTP_GET,
                            .handler = root_handler,
                            .user_ctx = NULL};
    httpd_register_uri_handler(server, &uri_root);

    httpd_uri_t uri_jpg = {.uri = "/jpg",
                           .method = HTTP_GET,
                           .handler = jpg_handler,
                           .user_ctx = NULL};
    httpd_register_uri_handler(server, &uri_jpg);

    httpd_uri_t uri_stream = {.uri = "/stream",
                              .method = HTTP_GET,
                              .handler = stream_handler,
                              .user_ctx = NULL};
    httpd_register_uri_handler(server, &uri_stream);
  }
  return server;
}

static void start_camera(void) {
  camera_config_t config = {.pin_pwdn = PWDN_GPIO_NUM,
                            .pin_reset = RESET_GPIO_NUM,
                            .pin_xclk = XCLK_GPIO_NUM,
                            .pin_sccb_sda = SIOD_GPIO_NUM,
                            .pin_sccb_scl = SIOC_GPIO_NUM,
                            .pin_d7 = Y9_GPIO_NUM,
                            .pin_d6 = Y8_GPIO_NUM,
                            .pin_d5 = Y7_GPIO_NUM,
                            .pin_d4 = Y6_GPIO_NUM,
                            .pin_d3 = Y5_GPIO_NUM,
                            .pin_d2 = Y4_GPIO_NUM,
                            .pin_d1 = Y3_GPIO_NUM,
                            .pin_d0 = Y2_GPIO_NUM,
                            .pin_vsync = VSYNC_GPIO_NUM,
                            .pin_href = HREF_GPIO_NUM,
                            .pin_pclk = PCLK_GPIO_NUM,

                            .xclk_freq_hz = 20000000,
                            .ledc_timer = LEDC_TIMER_0,
                            .ledc_channel = LEDC_CHANNEL_0,

                            .pixel_format = PIXFORMAT_JPEG,
                            .frame_size = FRAMESIZE_QVGA,
                            .jpeg_quality = 12,
                            .fb_count = 2,
                            .grab_mode = CAMERA_GRAB_WHEN_EMPTY};

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
  } else {
    ESP_LOGI(TAG, "Camera init succeeded");
  }
}

// Event handler for Wi-Fi events
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "Client connected: MAC=" MACSTR ", AID=%d",
             MAC2STR(event->mac), event->aid);
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "Client disconnected: MAC=" MACSTR ", AID=%d",
             MAC2STR(event->mac), event->aid);
  }
}

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
  assert(ap_netif);

  // Register Wi-Fi event handler
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

  wifi_config_t ap_config = {.ap = {.ssid = "ESP32-CAM",
                                    .ssid_len = strlen("ESP32-CAM"),
                                    .channel = 1,
                                    .password = "12345678",
                                    .max_connection = 4,
                                    .authmode = WIFI_AUTH_WPA_WPA2_PSK}};
  if (strlen("12345678") == 0) {
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  // Log the AP IP address
  esp_netif_ip_info_t ip_info;
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
    ESP_LOGI(TAG, "AP started, IP Address: " IPSTR, IP2STR(&ip_info.ip));
  }

  start_camera();
  start_webserver();
}