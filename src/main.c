#include <stdio.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>

#define SENSOR_PIN GPIO_NUM_0 // GPIO0 för Hall-sensorn

#define WIFI_SSID "Bonet"
#define WIFI_PASS "vbpB73074"
#define SERVER_URL "http://192.168.88.118:8080/api/machine-data"
#define REGISTER_URL "http://192.168.88.118:8080/api/register-device"

// Device registration variables
static int device_id = 0;         // 0 means not registered yet
static char device_name[64] = ""; // Empty until assigned by server
static char firmware_version[32] = "1.0.0";
static TaskHandle_t registration_task_handle = NULL; // ADD THIS LINE

// Function prototypes
static bool register_device(void);
static void save_device_info(void);
static bool load_device_info(void);
static void send_sensor_data(int sensor_value);
static void initialize_sntp(void); // ADD THIS LINE
// ADD THIS ENTIRE FUNCTION HERE:
static void registration_task(void *pvParameters)
{
    // Wait a bit for WiFi to stabilize
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (device_id == 0)
    {
        printf("Device not registered, attempting registration...\n");
        if (register_device())
        {
            printf("Device registered successfully! ID: %d, Name: %s\n", device_id, device_name);
        }
        else
        {
            printf("Device registration failed, will retry later\n");
        }
    }

    // Delete this task when done
    registration_task_handle = NULL;
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
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

        // Initialize time synchronization
        initialize_sntp();

        // Create registration task
        if (device_id == 0 && registration_task_handle == NULL)
        {
            xTaskCreate(registration_task, "registration", 8192, NULL, 5, &registration_task_handle);
        }
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char response_buffer[512];
    static int response_index = 0;

    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        printf("HTTP_EVENT_ERROR\n");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        printf("HTTP_EVENT_ON_CONNECTED\n");
        response_index = 0;
        break;
    case HTTP_EVENT_HEADERS_SENT:
        printf("HTTP_EVENT_HEADERS_SENT\n");
        break;
    case HTTP_EVENT_ON_HEADER:
        printf("HTTP_EVENT_ON_HEADER, key=%s, value=%s\n", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        printf("HTTP_EVENT_ON_DATA, len=%d\n", evt->data_len);
        if (evt->data_len < (sizeof(response_buffer) - response_index - 1))
        {
            memcpy(response_buffer + response_index, evt->data, evt->data_len);
            response_index += evt->data_len;
            response_buffer[response_index] = '\0';
            printf("Received data chunk: %.*s\n", evt->data_len, (char *)evt->data);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        printf("HTTP_EVENT_ON_FINISH\n");
        printf("Complete response: %s\n", response_buffer);

        // Parse the response here
        char *id_ptr = strstr(response_buffer, "\"device_id\":");
        if (id_ptr)
        {
            id_ptr += 12;
            device_id = atoi(id_ptr);
            printf("Parsed device_id: %d\n", device_id);
        }
        else
        {
            device_id = 64; // fallback
        }

        char *name_ptr = strstr(response_buffer, "\"device_name\":\"");
        if (name_ptr)
        {
            name_ptr += 15;
            char *name_end = strchr(name_ptr, '"');
            if (name_end)
            {
                int name_len = name_end - name_ptr;
                if (name_len < sizeof(device_name))
                {
                    strncpy(device_name, name_ptr, name_len);
                    device_name[name_len] = '\0';
                }
            }
        }
        else
        {
            strcpy(device_name, "Weaving-Machine-ESP32");
        }
        break;
    case HTTP_EVENT_DISCONNECTED:
        printf("HTTP_EVENT_DISCONNECTED\n");
        break;
    case HTTP_EVENT_REDIRECT: // ADD THIS CASE
        printf("HTTP_EVENT_REDIRECT\n");
        break;
    }
    return ESP_OK;
}
static bool register_device(void)
{
    printf("=== REGISTERING DEVICE ===\n");

    char json_data[256];
    snprintf(json_data, sizeof(json_data),
             "{"
             "\"firmware_version\":\"%s\","
             "\"device_type\":\"ESP32-C6\","
             "\"capabilities\":[\"hall_sensor\",\"wifi\"]"
             "}",
             firmware_version);

    printf("Registration JSON: %s\n", json_data);

    esp_http_client_config_t config = {
        .url = REGISTER_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_data, strlen(json_data));

    esp_err_t err = esp_http_client_perform(client);
    bool success = false;

    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        printf("Registration Status = %d\n", status_code);

        if (status_code == 200)
        {
            printf("Final device info: ID=%d, Name=%s\n", device_id, device_name);
            save_device_info();
            success = true;
            printf("Registration completed successfully!\n");
        }
    }
    else
    {
        printf("Registration failed: %s\n", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}

// Save device info to NVS
static void save_device_info(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("device", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        nvs_set_i32(nvs_handle, "device_id", device_id);
        nvs_set_str(nvs_handle, "device_name", device_name);
        nvs_set_str(nvs_handle, "firmware_ver", firmware_version);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        printf("Device info saved to NVS\n");
    }
    else
    {
        printf("Failed to save device info: %s\n", esp_err_to_name(err));
    }
}

static bool load_device_info(void)
{
    printf("=== ATTEMPTING TO LOAD DEVICE INFO FROM NVS ===\n");

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("device", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        printf("No saved device info found - NVS open failed: %s\n", esp_err_to_name(err));
        return false;
    }

    printf("NVS opened successfully, attempting to read device_id...\n");

    size_t required_size;

    // Load device_id
    int32_t temp_device_id;
    err = nvs_get_i32(nvs_handle, "device_id", &temp_device_id);
    printf("nvs_get_i32 result: %s\n", esp_err_to_name(err));

    if (err == ESP_OK)
    {
        device_id = temp_device_id;
        printf("Successfully loaded device_id: %d\n", device_id);
    }
    else
    {
        device_id = 0;
        printf("Failed to load device_id, set to 0\n");
    }

    // Load device_name
    required_size = sizeof(device_name);
    err = nvs_get_str(nvs_handle, "device_name", device_name, &required_size);
    printf("nvs_get_str result for device_name: %s\n", esp_err_to_name(err));

    if (err != ESP_OK)
    {
        strcpy(device_name, "");
        printf("Failed to load device_name, set to empty\n");
    }
    else
    {
        printf("Successfully loaded device_name: %s\n", device_name);
    }

    nvs_close(nvs_handle);

    if (device_id > 0)
    {
        printf("=== DEVICE INFO LOADED: ID=%d, Name=%s ===\n", device_id, device_name);
        return true;
    }
    else
    {
        printf("=== NO VALID DEVICE INFO FOUND ===\n");
        return false;
    }
}

static void send_sensor_data(int sensor_value)
{
    printf("=== ATTEMPTING TO SEND DATA ===\n");

    if (device_id == 0)
    {
        printf("Device not registered yet, skipping data send\n");
        return;
    }

    // Get current real time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Create timestamp in one step with larger buffer
    char final_timestamp[64];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int ms = tv.tv_usec / 1000;

    // Format everything at once
    snprintf(final_timestamp, sizeof(final_timestamp),
             "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, ms);

    // Rest of your JSON code...
    char json_data[256];
    snprintf(json_data, sizeof(json_data),
             "{"
             "\"machine_id\":%d,"
             "\"timestamp\":\"%s\","
             "\"event_type\":\"production\","
             "\"value\":%d,"
             "\"fabric_id\":1"
             "}",
             device_id,
             final_timestamp, // Use real timestamp
             sensor_value);

    printf("JSON: %s\n", json_data);
    // ... rest of HTTP code

    printf("Server URL: %s\n", SERVER_URL);

    // HTTP client configuration
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
        printf("HTTP POST Status = %d\n", esp_http_client_get_status_code(client));
    }
    else
    {
        printf("HTTP POST request failed: %s\n", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

// Add this function for one-time clearing
void factory_reset_device(void)
{
    printf("=== COMPLETE FLASH PARTITION ERASE ===\n");

    // Find and erase the NVS partition completely
    const esp_partition_t *nvs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (nvs_partition != NULL)
    {
        printf("Found NVS partition at offset 0x%lx, size 0x%lx\n", nvs_partition->address, nvs_partition->size);
        esp_err_t err = esp_partition_erase_range(nvs_partition, 0, nvs_partition->size);
        if (err == ESP_OK)
        {
            printf("NVS partition completely erased!\n");
        }
        else
        {
            printf("Failed to erase NVS partition: %s\n", esp_err_to_name(err));
        }
    }

    // Reset device variables
    device_id = 0;
    strcpy(device_name, "");

    printf("=== NVS COMPLETELY WIPED ===\n");
}

static void initialize_sntp(void)
{
    printf("Initializing SNTP time sync...\n");

    // Set timezone first - this looks like Stockholm/Central European Time
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_init();

    // Wait for time to be set
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

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

void app_main(void)
{
    // factory_reset_device();
    // return;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load saved device info from NVS
    bool device_loaded = load_device_info();
    if (device_loaded)
    {
        printf("Device already registered: ID=%d, Name=%s\n", device_id, device_name);
    }
    else
    {
        printf("Device not registered yet, will register when WiFi connects\n");
    }

    // ... rest of your existing WiFi initialization code ...

    // Initialize WiFi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Register event handler
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Initialize WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Configure WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Konfigurera GPIO0 som ingång med intern pull-up
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io_conf);

    int previous_level = gpio_get_level(SENSOR_PIN); // Initialize with current state
    int transition_count = 0;
    int64_t last_send_time = esp_timer_get_time();

    while (1)
    {
        int current_level = gpio_get_level(SENSOR_PIN);

        // Debug: Print sensor readings every 100 loops (every 2 seconds)
        static int debug_counter = 0;
        debug_counter++;
        if (debug_counter >= 100)
        {
            printf("DEBUG: current_level=%d, previous_level=%d\n", current_level, previous_level);
            debug_counter = 0;
        }

        // Detect 0->1 transition (rising edge)
        if (previous_level == 0 && current_level == 1)
        {
            transition_count++;
            printf("*** TRANSITION DETECTED! previous=%d -> current=%d, Count: %d ***\n",
                   previous_level, current_level, transition_count);
        }

        // Debug: Log any change in sensor value
        if (previous_level != current_level)
        {
            printf("SENSOR CHANGE: %d -> %d (but not a 0->1 transition)\n",
                   previous_level, current_level);
        }

        previous_level = current_level;

        // Check if 10 seconds have passed
        if ((esp_timer_get_time() - last_send_time) >= 10000000)
        { // 10 seconds in microseconds
            printf("Sending data: %d transitions in 10 seconds\n", transition_count);
            send_sensor_data(transition_count);
            transition_count = 0;
            last_send_time = esp_timer_get_time();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
