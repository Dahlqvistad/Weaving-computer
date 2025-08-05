#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_https_ota.h"

#define SENSOR_PIN GPIO_NUM_0
#define WIFI_SSID "Bonet"
#define WIFI_PASS "vbpB73074"
#define SERVER_URL "http://192.168.88.118:8080/api/machine-data"
#define REGISTER_URL "http://192.168.88.118:8080/api/register-device"
#define CHECK_UPDATE_URL "http://192.168.88.118:8080/api/check-update/%d?current_version=%s"

// Device registration variables
static int device_id = 0;
static char firmware_version[16] = "1.0.2"; // THIS IS v1.0.1!
static TaskHandle_t registration_task_handle = NULL;

// Function prototypes
static bool register_device(void);
static void save_device_info(void);
static bool load_device_info(void);
static void send_sensor_data(int sensor_value);
static void initialize_sntp(void);
static void check_for_updates(void);
static void perform_ota_update(const char *url);
static void ota_task(void *pvParameters);

static void registration_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (device_id == 0)
    {
        register_device();
    }

    registration_task_handle = NULL;
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        printf("Retry connecting to WiFi\n");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        printf("WiFi connected successfully!\n");

        initialize_sntp();

        if (device_id == 0 && registration_task_handle == NULL)
        {
            xTaskCreate(registration_task, "registration", 8192, NULL, 5, &registration_task_handle);
        }

        static TaskHandle_t ota_task_handle = NULL;
        if (ota_task_handle == NULL)
        {
            xTaskCreate(ota_task, "ota", 6144, NULL, 3, &ota_task_handle);
        }
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char response_buffer[128];
    static int response_index = 0;

    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        break;
    case HTTP_EVENT_ON_CONNECTED:
        response_index = 0;
        break;
    case HTTP_EVENT_ON_HEADER:
        break;
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len < (sizeof(response_buffer) - response_index - 1))
        {
            memcpy(response_buffer + response_index, evt->data, evt->data_len);
            response_index += evt->data_len;
            response_buffer[response_index] = '\0';
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        char *id_ptr = strstr(response_buffer, "\"device_id\":");
        if (id_ptr)
        {
            id_ptr += 12;
            device_id = atoi(id_ptr);
        }
        else
        {
            device_id = 64;
        }
        break;
    case HTTP_EVENT_DISCONNECTED:
        break;
    default:
        break;
    }
    return ESP_OK;
}

static bool register_device(void)
{
    char json[128];
    snprintf(json, sizeof(json),
             "{\"firmware_version\":\"%s\",\"device_type\":\"ESP32-C6\"}",
             firmware_version);

    esp_http_client_config_t config = {
        .url = REGISTER_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    bool success = (err == ESP_OK && esp_http_client_get_status_code(client) == 200);

    if (success)
        save_device_info();

    esp_http_client_cleanup(client);
    return success;
}

static void save_device_info(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open("device", NVS_READWRITE, &nvs_handle) == ESP_OK)
    {
        nvs_set_i32(nvs_handle, "device_id", device_id);
        nvs_set_str(nvs_handle, "firmware_ver", firmware_version);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

static bool load_device_info(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open("device", NVS_READONLY, &nvs_handle) != ESP_OK)
        return false;

    int32_t temp_device_id;
    if (nvs_get_i32(nvs_handle, "device_id", &temp_device_id) == ESP_OK)
    {
        device_id = temp_device_id;
    }
    else
    {
        device_id = 0;
    }

    nvs_close(nvs_handle);
    return (device_id > 0);
}

static void send_sensor_data(int sensor_value)
{
    if (device_id == 0)
        return;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char final_timestamp[64];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int ms = tv.tv_usec / 1000;

    snprintf(final_timestamp, sizeof(final_timestamp),
             "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, ms);

    char json_data[256];
    snprintf(json_data, sizeof(json_data),
             "{"
             "\"machine_id\":%d,"
             "\"timestamp\":\"%s\","
             "\"event_type\":\"production\","
             "\"value\":%d,"
             "\"fabric_id\":1"
             "}",
             device_id, final_timestamp, sensor_value);

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_data, strlen(json_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        printf("Data sent: %d\n", sensor_value);
    }
    esp_http_client_cleanup(client);
}

static void initialize_sntp(void)
{
    printf("Initializing SNTP time sync...\n");

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 11;

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count)
    {
        printf("Waiting for system time to be set... (%d/%d)\n", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (retry < retry_count)
    {
        printf("Time synchronized! Current local time: %s", asctime(&timeinfo));
    }
    else
    {
        printf("Failed to sync time, using relative timestamps\n");
    }
}

static void check_for_updates(void)
{
    if (device_id == 0)
        return;

    char check_url[256];
    snprintf(check_url, sizeof(check_url), CHECK_UPDATE_URL, device_id, firmware_version);

    esp_http_client_config_t config = {
        .url = check_url,
        .method = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200)
    {
        char response[512] = {0};
        int data_read = esp_http_client_read(client, response, sizeof(response) - 1);

        if (data_read > 0 && strstr(response, "\"update_available\":true"))
        {
            char *url_start = strstr(response, "\"download_url\":\"");
            if (url_start)
            {
                url_start += 16;
                char *url_end = strchr(url_start, '"');
                if (url_end)
                {
                    int url_len = url_end - url_start;
                    if (url_len < 200)
                    {
                        char download_url[256];
                        strncpy(download_url, url_start, url_len);
                        download_url[url_len] = '\0';
                        perform_ota_update(download_url);
                    }
                }
            }
        }
    }

    esp_http_client_cleanup(client);
}

static void perform_ota_update(const char *url)
{
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 30000,
        .skip_cert_common_name_check = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}

static void ota_task(void *pvParameters)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(60000)); // 1 minute for testing
        if (device_id > 0)
        {
            check_for_updates();
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    bool device_loaded = load_device_info();
    if (device_loaded)
    {
        printf("Device already registered: ID=%d\n", device_id);
    }
    else
    {
        printf("Device not registered yet, will register when WiFi connects\n");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io_conf);

    int previous_level = gpio_get_level(SENSOR_PIN);
    int transition_count = 0;
    int64_t last_send_time = esp_timer_get_time();
    int64_t last_transition_time = 0;
    const int64_t debounce_time = 50000;

    printf("Starting sensor monitoring with debouncing...\n");
    printf("Initial sensor level: %d\n", previous_level);

    while (1)
    {
        int current_level = gpio_get_level(SENSOR_PIN);
        int64_t current_time = esp_timer_get_time();

        if (current_level == 1 && previous_level == 0)
        {
            if ((current_time - last_transition_time) > debounce_time)
            {
                transition_count++;
                last_transition_time = current_time;
            }
        }

        previous_level = current_level;

        // 20 SECOND INTERVAL FOR v1.0.1!!!
        if ((current_time - last_send_time) >= 20000000)
        {
            send_sensor_data(transition_count);
            transition_count = 0;
            last_send_time = current_time;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
