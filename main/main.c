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
#include "esp_littlefs.h"
#include "driver/gpio.h"

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

// Pin map for track control

#define LEFT_FWD 5
#define LEFT_REV 2
#define RIGHT_FWD 3
#define RIGHT_REV 4

typedef struct {
    int pin;
    int channel;
    int freq_hz;
    int fade_delay_ms;
} pwm_blink_config_t;

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
    FILE *f = fopen("/www/index.html", "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");

    char buffer[512];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL); // end response
            return ESP_FAIL;
        }
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // end response
    return ESP_OK;
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
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
      s->set_whitebal(s, 1); // Enable AWB
      s->set_gain_ctrl(s, 1); // Enable auto gain
      s->set_exposure_ctrl(s, 1); // Enable auto exposure
      ESP_LOGI(TAG, "Auto white balance, gain, and exposure enabled");
    }
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

static void init_filesystem(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount or format LittleFS (%s)", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_littlefs_info(conf.partition_label, &total, &used);
        ESP_LOGI(TAG, "LittleFS mounted, total: %d, used: %d", total, used);
    }
}

void pwm_blink_task(void *pvParameter)
{
    pwm_blink_config_t cfg = *(pwm_blink_config_t *)pvParameter;
    free(pvParameter);  // free allocated config after copy

    while (1) {
        // Fade up
        for (int duty = 0; duty <= 8191; duty += 128) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, cfg.channel, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, cfg.channel);
            vTaskDelay(pdMS_TO_TICKS(cfg.fade_delay_ms));
        }
        // Fade down
        for (int duty = 8191; duty >= 0; duty -= 128) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, cfg.channel, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, cfg.channel);
            vTaskDelay(pdMS_TO_TICKS(cfg.fade_delay_ms));
        }
    }
}

void start_pwm_blink(int pin, int channel, int freq_hz, int fade_delay_ms)
{
    if (channel >= 8) {
        printf("Error: only %d channels supported\n", 8);
        return;
    }

    // Configure timer (one timer can be shared)
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_13_BIT,  // 0–8191
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = freq_hz,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    // Configure channel
    ledc_channel_config_t ledc_channel = {
        .gpio_num   = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = channel,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ledc_channel);

    // Prepare config for the task
    pwm_blink_config_t *cfg = malloc(sizeof(pwm_blink_config_t));
    cfg->pin = pin;
    cfg->channel = channel;
    cfg->freq_hz = freq_hz;
    cfg->fade_delay_ms = fade_delay_ms;

    // Launch async task
    xTaskCreate(pwm_blink_task, "pwm_blink_task", 2048, cfg, 5, NULL);
}

void app_main(void) {
  uint64_t pin_mask = (1ULL << LEFT_FWD) | (1ULL << LEFT_REV) | (1ULL << RIGHT_FWD) | (1ULL << RIGHT_REV);

  gpio_config_t io_conf = {
      .pin_bit_mask = pin_mask,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io_conf);
  //start_pwm_blink(RIGHT_FWD, 0, 5000, 20);

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  init_filesystem();

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