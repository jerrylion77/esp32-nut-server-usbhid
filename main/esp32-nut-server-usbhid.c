/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "errno.h"
#include "driver/gpio.h"

#include "usb/hid_host.h"

#include "driver/gptimer.h"
#include "freertos/queue.h"

#include "sys/socket.h"
#include "netdb.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "led_strip.h"

#include "cJSON.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "ups_models_config.h"

#include <inttypes.h>

#include "esp_timer.h"

#include "webserver.h"

#include "esp_http_client.h"

// === WiFi NVS Management ===
#define WIFI_NVS_NAMESPACE "wifi_config"
#define WIFI_SSID_KEY "ssid"
#define WIFI_PASS_KEY "password"

// === WiFi NVS Management Function Prototypes ===
static esp_err_t load_wifi_credentials_from_nvs(char* ssid, char* password, size_t max_len);

// === Button Function Prototypes ===
static void button_monitor_task(void* pvParameters);

// === LED Function Prototypes ===
static void configure_led(void);
static void set_led_green(void);
static void set_led_yellow(void);
static void set_led_red(void);
static void set_led_white(void);
static void set_led_purple(void);
static void set_led_blue(void);
static void update_led_status(void);
static void update_led_with_pulse(void);
static void pulse_timer_callback(void* arg);

// === UPS Data Storage and State Management ===
#include <stdbool.h>
#include <stdint.h>

#define UPS_DATA_FRESHNESS_TIMEOUT_MS 10000  // 10 seconds for data freshness

// UPS state enum
typedef enum {
    UPS_DISCONNECTED = 0,
    UPS_CONNECTED_WAITING_DATA,
    UPS_CONNECTED_ACTIVE,
    UPS_CONNECTED_STALE
} ups_connection_state_t;

// UPS data storage struct (17 fields)
typedef struct {
    int battery_level;
    int battery_byte2;
    int battery_byte3;
    int status;
    int status_byte2;
    int runtime;
    int input_voltage;
    int output_voltage;
    int load;
    int alarm_control;
    int beep_control;
    int system_status;
    int extended_status;
    int temperature;
    int temp_range1;
    int temp_range2;
    int additional_sensor;
} ups_data_store_t;

// Global UPS state
static ups_connection_state_t ups_state = UPS_DISCONNECTED;
static ups_data_store_t ups_data = {0};
static uint32_t ups_last_data_time = 0;  // ms since boot
static bool ups_available = false;

// --- LED Pulse Tracking Variables (Cosmetic, Safe to Remove) ---
// These are only used for the RGB LED status indicator. If you want to disable LED logic,
// you can comment out or remove all code that references these variables and the related functions.
static uint32_t last_field_update_time = 0;  // ms since boot
static uint32_t last_pulse_time = 0;         // ms since boot
static bool pulse_in_progress = false;
static const uint32_t PULSE_INTERVAL_MS = 30000;  // 30 seconds
static const uint32_t PULSE_DURATION_MS = 1000;   // 1 second white flash
// --- End LED Pulse Tracking Variables ---

// Background timer task for UPS data freshness checking
static void ups_freshness_timer_task(void *pvParameters)
{
    const char *TAG = "ups-timer";
    ESP_LOGI(TAG, "UPS freshness timer task started");
    
    TickType_t last_log = xTaskGetTickCount();
    while (1) {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t time_since_last_data = current_time - ups_last_data_time;
        
        // Check if UPS data is stale (no data for more than 10 seconds)
        if (ups_state == UPS_CONNECTED_ACTIVE && time_since_last_data > UPS_DATA_FRESHNESS_TIMEOUT_MS) {
            ups_state = UPS_CONNECTED_STALE;
            ups_available = false;
            ESP_LOGW(TAG, "UPS state: ACTIVE -> STALE (no data for %lu ms)", time_since_last_data);
            update_led_with_pulse();  // Update LED when UPS becomes stale
        }
        
        // Log current state every 30 seconds for debugging
        static uint32_t last_log_time = 0;
        if (current_time - last_log_time > 30000) {  // 30 seconds
            if (ups_state == UPS_DISCONNECTED) {
                ESP_LOGI(TAG, "UPS Timer Check - State: %d, Available: %s, UPS Disconnected", 
                         ups_state, ups_available ? "YES" : "NO");
            } else {
                ESP_LOGI(TAG, "UPS Timer Check - State: %d, Available: %s, Last Data: %lu ms ago", 
                         ups_state, ups_available ? "YES" : "NO", time_since_last_data);
            }
            last_log_time = current_time;
        }
        
        // Check every 2 seconds
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Periodically log stack high water mark
        if (xTaskGetTickCount() - last_log > 30000 / portTICK_PERIOD_MS) {
            last_log = xTaskGetTickCount();
            ESP_LOGI("ups_timer", "Stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
        }
    }
}

// Global variable to hold the latest UPS data for NUT reporting (unused in current implementation)
// static ups_data_t latest_ups_data = {0};

// CyberPower UPS vendor/product IDs
#define CYBERPOWER_VENDOR_ID    0x0764
#define CYBERPOWER_VP700ELCD    0x0501
#define CYBERPOWER_VP1000ELCD   0x0502

// Generic HID UPS parsing definitions
#define MAX_UPS_MODELS 10
#define MAX_REPORT_SIZE 64

// Global variables for generic parsing
static ups_model_config_t ups_models[MAX_UPS_MODELS];
static uint8_t detected_model_index = 0xFF;  // 0xFF means no model detected
static bool model_detected = false;

// UPS filtering variables
static bool device_is_ups = false;
static bool waiting_for_initial_data = false;
static uint32_t device_connection_time = 0;
static const uint32_t UPS_DATA_TIMEOUT_MS = 1000;  // 1 second to wait for initial data
static hid_host_device_handle_t current_device_handle = NULL;

// Function declarations
static esp_err_t __attribute__((unused)) init_generic_ups_models(void);
// static esp_err_t detect_ups_model(hid_host_device_handle_t device_handle);  // REMOVED - unused
static esp_err_t parse_ups_data_generic(hid_host_device_handle_t device_handle, ups_data_t *data);
static esp_err_t set_beep_generic(hid_host_device_handle_t device_handle, bool enabled);
static uint8_t extract_field_value(const uint8_t *data, const hid_report_mapping_t *mapping);
static uint32_t extract_multi_byte_value(const uint8_t *data, const hid_report_mapping_t *mapping);
static void update_json_with_ups_data(const ups_data_t *data);
static void debug_unknown_ups_model(hid_host_device_handle_t device_handle);
static void refresh_ups_status_from_hid(bool *beep);

// Pulse timer callback function
static void pulse_timer_callback(void* arg)
{
    pulse_in_progress = false;
    update_led_status();  // Return to base color
}

/* It is use for change beep status */
#define APP_QUIT_PIN GPIO_NUM_0
#define RGB_LED_PIN GPIO_NUM_48

static led_strip_handle_t led_strip;

// Logging control - set to 0 to disable verbose UPS parsing logs
#define VERBOSE_UPS_LOGGING 1

static const char *TAG = "ups";
QueueHandle_t hid_host_event_queue;
QueueHandle_t timer_queue;
typedef struct
{
    uint64_t event_count;
} timer_queue_element_t;
bool user_shutdown = false;
bool UPS_DEV_CONNECTED = false;
hid_host_device_handle_t latest_hid_device_handle;

// Store debug data for CyberPower UPS (unused in current implementation)
// static uint8_t debug_report_data[16][MAX_REPORT_SIZE];
// static size_t debug_report_lengths[16];
// static bool debug_data_available = false;

// Store live HID data for UPS parsing (unused in current implementation)
// static uint8_t live_hid_data[256][MAX_REPORT_SIZE];  // Support all possible report IDs (0x00-0xFF)
// static size_t live_hid_lengths[256];
// static bool live_data_available = false;
// static uint32_t last_live_data_time = 0;

/**
 * @brief HID Host event
 *
 * This event is used for delivering the HID Host event from callback to a task.
 */
typedef struct
{
    hid_host_device_handle_t hid_device_handle;
    hid_host_driver_event_t event;
    void *arg;
} hid_host_event_queue_t;

/**
 * @brief HID Protocol string names
 */
static const char *hid_proto_name_str[] = {
    "NONE",
    "KEYBOARD",
    "MOUSE"};

// =tcp server

static char *ori_json = "{\"battery\":{\"charge\":{\"_root\":\"100\",\"low\":\"20\"},\"charger\":{\"status\":\"charging\"},\"runtime\":\"1104\",\"type\":\"PbAc\"},\"device\":{\"mfr\":\"EATON\",\"model\":\"SANTAK TG-BOX 850\",\"serial\":\"Blank\",\"type\":\"ups\"},\"driver\":{\"name\":\"usbhid-ups\",\"parameter\":{\"pollfreq\":30,\"pollinterval\":2,\"port\":\"/dev/ttyS1\",\"synchronous\":\"no\"},\"version\":{\"_root\":\"2.7.4\",\"data\":\"MGE HID 1.39\",\"internal\":\"0.41\"}},\"input\":{\"transfer\":{\"high\":\"264\",\"low\":\"184\"}},\"outlet\":{\"1\":{\"desc\":\"PowerShare Outlet 1\",\"id\":\"1\",\"status\":\"on\",\"switchable\":\"no\"},\"desc\":\"Main Outlet\",\"id\":\"0\",\"switchable\":\"yes\"},\"output\":{\"frequency\":{\"nominal\":\"50\"},\"voltage\":{\"_root\":\"230.0\",\"nominal\":\"220\"}},\"ups\":{\"beeper\":{\"status\":\"enabled\"},\"delay\":{\"shutdown\":\"20\",\"start\":\"30\"},\"firmware\":\"02.08.0010\",\"load\":\"28\",\"mfr\":\"EATON\",\"model\":\"SANTAK TG-BOX 850\",\"power\":{\"nominal\":\"850\"},\"productid\":\"ffff\",\"serial\":\"Blank\",\"status\":\"OL\",\"timer\":{\"shutdown\":\"0\",\"start\":\"0\"},\"type\":\"offline / line interactive\",\"vendorid\":\"0463\"}}";
cJSON *json_object;

char nut_list_var_text[2048]="";

void init_json_object()
{
    json_object = cJSON_Parse(ori_json);
}

void gen_nut_list_var_text(cJSON *input, char *parent_path)
{
    char *prefix_text = "VAR qnapups ";
    if (cJSON_IsString(input))
    {
        strcat(nut_list_var_text, parent_path);
        if (input->string[0] != '_')
        {
            strcat(nut_list_var_text, ".");
            strcat(nut_list_var_text, input->string);
        }
        strcat(nut_list_var_text, " \"");
        strcat(nut_list_var_text, input->valuestring);
        strcat(nut_list_var_text, "\"\n");
    }
    else if(cJSON_IsObject(input))
    {
        char new_parent_path[64] = "";
        strcpy(new_parent_path, parent_path);
        if (input->string != NULL)
        {
            if (strlen(new_parent_path) > strlen(prefix_text))
            {
                strcat(new_parent_path, ".");
            }
            strcat(new_parent_path, input->string);
        }
        else
        {
            strcat(new_parent_path, prefix_text);
        }
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, input)
        {
            gen_nut_list_var_text(child, new_parent_path);
        }
    }
}

/// @brief https://networkupstools.org/docs/developer-guide.chunked/ar01s09.html
void gen_nut_list_var_text_wrapper()
{
    strcpy(nut_list_var_text, "BEGIN LIST VAR qnapups\n");
    gen_nut_list_var_text(json_object, "");
    strcat(nut_list_var_text, "END LIST VAR qnapups\n");
}

bool str_startswith(const char *str, const char *p)
{
	int len = strlen(p);
	if (len <= 0)
		return 0;
	if (strncmp(str, p, len) == 0)
		return 1;

	return 0;
}

/**
 * @brief Indicates that the file descriptor represents an invalid (uninitialized or closed) socket
 *
 * Used in the TCP server structure `sock[]` which holds list of active clients we serve.
 */
#define INVALID_SOCK (-1)

/**
 * @brief Time in ms to yield to all tasks when a non-blocking socket would block
 *
 * Non-blocking socket operations are typically executed in a separate task validating
 * the socket status. Whenever the socket returns `EAGAIN` (idle status, i.e. would block)
 * we have to yield to all tasks to prevent lower priority tasks from starving.
 */
#define YIELD_TO_ALL_MS 50

/**
 * @brief Utility to log socket errors
 *
 * @param[in] tag Logging tag
 * @param[in] sock Socket number
 * @param[in] err Socket errno
 * @param[in] message Message to print
 */
static void log_socket_error(const char *tag, const int sock, const int err, const char *message)
{
    ESP_LOGE(tag, "[sock=%d]: %s\n"
                  "error=%d: %s", sock, message, err, strerror(err));
}

/**
 * @brief Tries to receive data from specified sockets in a non-blocking way,
 *        i.e. returns immediately if no data.
 *
 * @param[in] tag Logging tag
 * @param[in] sock Socket for reception
 * @param[out] data Data pointer to write the received data
 * @param[in] max_len Maximum size of the allocated space for receiving data
 * @return
 *          >0 : Size of received data
 *          =0 : No data available
 *          -1 : Error occurred during socket read operation
 *          -2 : Socket is not connected, to distinguish between an actual socket error and active disconnection
 */
static int __attribute__((unused)) try_receive(const char *tag, const int sock, char * data, size_t max_len)
{
    int len = recv(sock, data, max_len, 0);
    if (len < 0) {
        if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;   // Not an error
        }
        if (errno == ENOTCONN) {
            ESP_LOGW(tag, "[sock=%d]: Connection closed", sock);
            return -2;  // Socket has been disconnected
        }
        if (errno == ECONNRESET)
        {
            //will happen as nut design. Not an error when communicating with nut clients
            return 0;
        }
        
        log_socket_error(tag, sock, errno, "Error occurred during receiving");
        return -1;
    }

    return len;
}

/**
 * @brief Sends the specified data to the socket. This function blocks until all bytes got sent.
 *
 * @param[in] tag Logging tag
 * @param[in] sock Socket to write data
 * @param[in] data Data to be written
 * @param[in] len Length of the data
 * @return
 *          >0 : Size the written data
 *          -1 : Error occurred during socket write operation
 */



/**
 * @brief Returns the string representation of client's address (accepted on this server)
 */
static inline char* get_clients_address(struct sockaddr_storage *source_addr)
{
    static char address_str[128];
    char *res = NULL;
    // Convert ip address to string
    if (source_addr->ss_family == PF_INET) {
        res = inet_ntoa_r(((struct sockaddr_in *)source_addr)->sin_addr, address_str, sizeof(address_str) - 1);
    }
#ifdef CONFIG_LWIP_IPV6
    else if (source_addr->ss_family == PF_INET6) {
        res = inet6_ntoa_r(((struct sockaddr_in6 *)source_addr)->sin6_addr, address_str, sizeof(address_str) - 1);
    }
#endif
    if (!res) {
        address_str[0] = '\0'; // Returns empty string if conversion didn't succeed
    }
    return address_str;
}

// --- TCP Server Status Tracking ---
#include "freertos/task.h"

static TaskHandle_t tcp_server_task_handle = NULL;
static int active_connections_count = 0;

int get_active_tcp_connections(void) {
    return active_connections_count;
}

bool is_tcp_server_running(void) {
    return (tcp_server_task_handle != NULL && eTaskGetState(tcp_server_task_handle) != eDeleted);
}

// TCP Server Task
void tcp_server_task(void *pvParameters)
{
    static char rx_buffer[128];
    static const char *TAG = "tcp-svr";
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM };
    struct addrinfo *address_info;
    int listen_sock = INVALID_SOCK;
    const size_t max_socks = 4; // Allow up to 4 clients
    static int sock[4];

    for (int i = 0; i < max_socks; ++i) {
        sock[i] = INVALID_SOCK;
    }

    int res = getaddrinfo("0.0.0.0", "3493", &hints, &address_info);
    if (res != 0 || address_info == NULL) {
        ESP_LOGE(TAG, "couldn't get hostname for 0.0.0.0 getaddrinfo() returns %d, addrinfo=%p", res, address_info);
        vTaskDelete(NULL);
        return;
    }

    listen_sock = socket(address_info->ai_family, address_info->ai_socktype, address_info->ai_protocol);
    if (listen_sock < 0) {
        log_socket_error(TAG, listen_sock, errno, "Unable to create socket");
        free(address_info);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Listener socket created");

    int flags = fcntl(listen_sock, F_GETFL);
    if (fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_socket_error(TAG, listen_sock, errno, "Unable to set socket non blocking");
        close(listen_sock);
        free(address_info);
        vTaskDelete(NULL);
        return;
    }

    int err = bind(listen_sock, address_info->ai_addr, address_info->ai_addrlen);
    if (err != 0) {
        log_socket_error(TAG, listen_sock, errno, "Socket unable to bind");
        close(listen_sock);
        free(address_info);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket bound on 0.0.0.0:3493");

    err = listen(listen_sock, 1);
    if (err != 0) {
        log_socket_error(TAG, listen_sock, errno, "Error occurred during listen");
        close(listen_sock);
        free(address_info);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket listening");
    free(address_info);

    TickType_t last_log = xTaskGetTickCount();
    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int new_sock_index = 0;
        for (new_sock_index = 0; new_sock_index < max_socks; ++new_sock_index) {
            if (sock[new_sock_index] == INVALID_SOCK) {
                break;
            }
        }
        if (new_sock_index < max_socks) {
            sock[new_sock_index] = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
            if (sock[new_sock_index] >= 0) {
                ESP_LOGI(TAG, "[sock=%d]: Connection accepted from IP:%s", sock[new_sock_index], get_clients_address(&source_addr));
                int flags = fcntl(sock[new_sock_index], F_GETFL);
                fcntl(sock[new_sock_index], F_SETFL, flags | O_NONBLOCK);
                // Update active connection count
                active_connections_count = 0;
                for (int j = 0; j < max_socks; ++j) {
                    if (sock[j] != INVALID_SOCK) active_connections_count++;
                }
            }
        }
        for (int i = 0; i < max_socks; ++i) {
            if (sock[i] != INVALID_SOCK) {
                int len = try_receive(TAG, sock[i], rx_buffer, sizeof(rx_buffer));
                if (len < 0) {
                    ESP_LOGI(TAG, "[sock=%d]: try_receive() returned %d -> closing the socket", sock[i], len);
                    close(sock[i]);
                    sock[i] = INVALID_SOCK;
                    // Update active connection count
                    active_connections_count = 0;
                    for (int j = 0; j < max_socks; ++j) {
                        if (sock[j] != INVALID_SOCK) active_connections_count++;
                    }
                } else if (len > 0) {
                    // NUT protocol command parsing
                    rx_buffer[len] = '\0'; // Null-terminate for string ops
                    ESP_LOGI(TAG, "[NUT] RX from client: %.*s", len, rx_buffer);

                    // Trim trailing CR/LF
                    char *cmd = rx_buffer;
                    while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r')) {
                        cmd[--len] = '\0';
                    }

                    const char *response = NULL;
                    char response_buf[1024];
                    response_buf[0] = '\0';

                    // Check UPS availability
                    bool ups_found = (ups_state != UPS_DISCONNECTED && ups_state != UPS_CONNECTED_WAITING_DATA);

                    // LIST UPS
                    if (strcasecmp(cmd, "LIST UPS") == 0) {
                        if (ups_found) {
                            snprintf(response_buf, sizeof(response_buf),
                                "BEGIN LIST UPS\nUPS VP700ELCD \"CyberPower VP700ELCD\"\nEND LIST UPS\n");
                        } else {
                            snprintf(response_buf, sizeof(response_buf), "ERR UPS-NOT-FOUND\n");
                        }
                        response = response_buf;
                    }
                    // LIST VAR VP700ELCD
                    else if (strncasecmp(cmd, "LIST VAR VP700ELCD", 18) == 0) {
                        if (ups_found) {
                            // Return all 17 UPS variables
                            snprintf(response_buf, sizeof(response_buf),
                                "BEGIN LIST VAR VP700ELCD\n"
                                "VAR VP700ELCD battery.charge \"%d\"\n"
                                "VAR VP700ELCD battery.runtime \"%d\"\n"
                                "VAR VP700ELCD input.voltage \"%d\"\n"
                                "VAR VP700ELCD output.voltage \"%d\"\n"
                                "VAR VP700ELCD ups.load \"%d\"\n"
                                "VAR VP700ELCD ups.status \"%s\"\n"
                                "VAR VP700ELCD battery.temperature \"%d\"\n"
                                "VAR VP700ELCD device.mfr \"CyberPower\"\n"
                                "VAR VP700ELCD device.model \"VP700ELCD\"\n"
                                "VAR VP700ELCD device.type \"ups\"\n"
                                "VAR VP700ELCD ups.firmware \"1.0\"\n"
                                "VAR VP700ELCD battery.type \"PbAc\"\n"
                                "VAR VP700ELCD ups.power.nominal \"700\"\n"
                                "VAR VP700ELCD ups.status.flags \"%d\"\n"
                                "VAR VP700ELCD ups.system.status \"%d\"\n"
                                "VAR VP700ELCD ups.extended.status \"%d\"\n"
                                "VAR VP700ELCD ups.alarm.control \"%d\"\n"
                                "VAR VP700ELCD ups.beep.control \"%d\"\n"
                                "END LIST VAR VP700ELCD\n",
                                ups_data.battery_level,
                                ups_data.runtime,
                                ups_data.input_voltage,
                                ups_data.output_voltage,
                                ups_data.load,
                                ups_available ? "OL" : "UNKNOWN",
                                ups_data.temperature,
                                ups_data.status,
                                ups_data.system_status,
                                ups_data.extended_status,
                                ups_data.alarm_control,
                                ups_data.beep_control);
                        } else {
                            snprintf(response_buf, sizeof(response_buf), "ERR UPS-NOT-FOUND\n");
                        }
                        response = response_buf;
                    }
                    // GET VAR VP700ELCD <varname>
                    else if (strncasecmp(cmd, "GET VAR VP700ELCD ", 18) == 0) {
                        if (ups_found) {
                            char *var = cmd + 18;
                            if (strcasecmp(var, "battery.charge") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD battery.charge \"%d\"\n", ups_data.battery_level);
                            } else if (strcasecmp(var, "battery.runtime") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD battery.runtime \"%d\"\n", ups_data.runtime);
                            } else if (strcasecmp(var, "input.voltage") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD input.voltage \"%d\"\n", ups_data.input_voltage);
                            } else if (strcasecmp(var, "output.voltage") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD output.voltage \"%d\"\n", ups_data.output_voltage);
                            } else if (strcasecmp(var, "ups.load") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD ups.load \"%d\"\n", ups_data.load);
                            } else if (strcasecmp(var, "ups.status") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD ups.status \"%s\"\n", ups_available ? "OL" : "UNKNOWN");
                            } else if (strcasecmp(var, "battery.temperature") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD battery.temperature \"%d\"\n", ups_data.temperature);
                            } else if (strcasecmp(var, "device.mfr") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD device.mfr \"CyberPower\"\n");
                            } else if (strcasecmp(var, "device.model") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD device.model \"VP700ELCD\"\n");
                            } else if (strcasecmp(var, "device.type") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD device.type \"ups\"\n");
                            } else if (strcasecmp(var, "ups.firmware") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD ups.firmware \"1.0\"\n");
                            } else if (strcasecmp(var, "battery.type") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD battery.type \"PbAc\"\n");
                            } else if (strcasecmp(var, "ups.power.nominal") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD ups.power.nominal \"700\"\n");
                            } else if (strcasecmp(var, "ups.status.flags") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD ups.status.flags \"%d\"\n", ups_data.status);
                            } else if (strcasecmp(var, "ups.system.status") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD ups.system.status \"%d\"\n", ups_data.system_status);
                            } else if (strcasecmp(var, "ups.extended.status") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD ups.extended.status \"%d\"\n", ups_data.extended_status);
                            } else if (strcasecmp(var, "ups.alarm.control") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD ups.alarm.control \"%d\"\n", ups_data.alarm_control);
                            } else if (strcasecmp(var, "ups.beep.control") == 0) {
                                snprintf(response_buf, sizeof(response_buf),
                                    "VAR VP700ELCD ups.beep.control \"%d\"\n", ups_data.beep_control);
                            } else {
                                snprintf(response_buf, sizeof(response_buf), "ERR VAR-NOT-FOUND\n");
                            }
                        } else {
                            snprintf(response_buf, sizeof(response_buf), "ERR UPS-NOT-FOUND\n");
                        }
                        response = response_buf;
                    }
                    // Authentication commands (stubbed for Home Assistant compatibility)
                    else if (str_startswith(cmd, "USERNAME") || str_startswith(cmd, "PASSWORD") || str_startswith(cmd, "LOGIN")) {
                        snprintf(response_buf, sizeof(response_buf), "OK\n");
                        response = response_buf;
                    }
                    else if (str_startswith(cmd, "LOGOUT")) {
                        snprintf(response_buf, sizeof(response_buf), "OK Goodbye\n");
                        response = response_buf;
                    }
                    // Unknown command
                    else {
                        snprintf(response_buf, sizeof(response_buf), "ERR UNKNOWN-COMMAND\n");
                        response = response_buf;
                    }

                    int sent = send(sock[i], response, strlen(response), 0);
                    ESP_LOGI(TAG, "[sock=%d]: Sent response (%d bytes): %s", sock[i], sent, response);
                    if (sent < 0) {
                        ESP_LOGE(TAG, "[sock=%d]: Failed to send response: %s", sock[i], strerror(errno));
                        close(sock[i]);
                        sock[i] = INVALID_SOCK;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(YIELD_TO_ALL_MS));
        
        // Periodically log stack high water mark
        if (xTaskGetTickCount() - last_log > 30000 / portTICK_PERIOD_MS) {
            last_log = xTaskGetTickCount();
            ESP_LOGI("tcp_server", "Stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
        }
    }
    // Cleanup (should not reach here)
    if (listen_sock != INVALID_SOCK) {
        close(listen_sock);
    }
    for (int i = 0; i < max_socks; ++i) {
        if (sock[i] != INVALID_SOCK) {
            close(sock[i]);
        }
    }
    vTaskDelete(NULL);
    tcp_server_task_handle = NULL;
}

// =tcp server

/**
 * @brief Makes new line depending on report output protocol type
 *
 * @param[in] proto Current protocol to output
 */
// REMOVED - unused function
// static void hid_print_new_device_report_header(hid_protocol_t proto)
// {
//     static hid_protocol_t prev_proto_output = -1;
// 
//     if (prev_proto_output != proto)
//     {
//         prev_proto_output = proto;
//         printf("\r\n");
//         if (proto == HID_PROTOCOL_MOUSE)
//         {
//             printf("Mouse\r\n");
//         }
//         else if (proto == HID_PROTOCOL_KEYBOARD)
//         {
//             printf("Keyboard\r\n");
//         }
//         else
//         {
//             printf("Generic\r\n");
//         }
//         fflush(stdout);
//     }
// }

/**
 * @brief USB HID Host Generic Interface report callback handler
 *
 * 'generic' means anything else than mouse or keyboard
 *
 * @param[in] data    Pointer to input report data buffer
 * @param[in] length  Length of input report data buffer
 */
// Global variable to store current UPS data
// static ups_data_t current_ups_data = {0};  // Unused in current implementation

static void hid_host_generic_report_callback(const uint8_t *const data, const int length)
{
    if (length < 1) {
        ESP_LOGW(TAG, "Received empty HID report");
        return;
    }
    
    uint8_t report_id = data[0];
    
    // Update UPS state and timestamp
    ups_last_data_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    last_field_update_time = ups_last_data_time;  // Track field update time for LED pulse
    
    if (ups_state == UPS_DISCONNECTED || ups_state == UPS_CONNECTED_WAITING_DATA) {
        ups_state = UPS_CONNECTED_ACTIVE;
        ups_available = true;
        ESP_LOGI(TAG, "UPS state: DISCONNECTED/WAITING -> ACTIVE");
        update_led_with_pulse();  // Update LED with pulse logic when UPS becomes active
    } else if (ups_state == UPS_CONNECTED_STALE) {
        ups_state = UPS_CONNECTED_ACTIVE;
        ESP_LOGI(TAG, "UPS state: STALE -> ACTIVE");
        update_led_with_pulse();  // Update LED with pulse logic when UPS becomes active
    } else {
        // UPS was already active, just update LED with pulse logic
        update_led_with_pulse();
    }
    
#if VERBOSE_UPS_LOGGING
    ESP_LOGI(TAG, "=== PARSING REPORT 0x%02X (Length: %d) ===", report_id, length);
    
    // Display raw data first
    ESP_LOGI(TAG, "Raw data:");
    for (int i = 0; i < length && i < 16; i++) {
        printf("%02X ", data[i]);
    }
    if (length > 16) printf("...");
    printf("\n");
#endif
    
    // Parse ALL reports with smart length handling
    switch (report_id) {
        case 0x20:  // Battery and Load Status
            ESP_LOGI(TAG, "Report 0x20 - Battery/Status Data:");
            if (length >= 2) {
                ups_data.battery_level = data[1];
                ESP_LOGI(TAG, "  Battery Level: %d%%", ups_data.battery_level);
            }
            if (length >= 3) {
                ups_data.battery_byte2 = data[2];
                ESP_LOGI(TAG, "  Battery Byte2: %d", ups_data.battery_byte2);
            }
            if (length >= 4) {
                ups_data.battery_byte3 = data[3];
                ESP_LOGI(TAG, "  Battery Byte3: %d", ups_data.battery_byte3);
            }
            break;
            
        case 0x21:  // Status Flags
            ESP_LOGI(TAG, "Report 0x21 - Status Flags:");
            if (length >= 2) {
                ups_data.status = data[1];
                ESP_LOGI(TAG, "  Status: %d", ups_data.status);
            }
            if (length >= 3) {
                ups_data.status_byte2 = data[2];
                ESP_LOGI(TAG, "  Status Byte2: %d", ups_data.status_byte2);
            }
            break;
            
        case 0x22:  // Runtime
            ESP_LOGI(TAG, "Report 0x22 - Runtime Data:");
            if (length >= 2) {
                ups_data.runtime = data[1];
                ESP_LOGI(TAG, "  Runtime: %d minutes", ups_data.runtime);
            }
            break;
            
        case 0x23:  // Voltage Data
            ESP_LOGI(TAG, "Report 0x23 - Voltage Data:");
            if (length >= 3) {
                ups_data.input_voltage = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Input Voltage: %d V", ups_data.input_voltage);
            }
            if (length >= 5) {
                ups_data.output_voltage = (data[4] << 8) | data[3];
                ESP_LOGI(TAG, "  Output Voltage: %d V", ups_data.output_voltage);
            }
            break;
            
        case 0x25:  // Load Percentage
            ESP_LOGI(TAG, "Report 0x25 - Load Data:");
            if (length >= 2) {
                ups_data.load = data[1];
                ESP_LOGI(TAG, "  Load: %d%%", ups_data.load);
            }
            break;
            
        case 0x28:  // Alarm Control
            ESP_LOGI(TAG, "Report 0x28 - Alarm Control:");
            if (length >= 2) {
                ups_data.alarm_control = data[1];
                ESP_LOGI(TAG, "  Alarm Control: %d", ups_data.alarm_control);
            }
            break;
            
        case 0x29:  // Beep Control
            ESP_LOGI(TAG, "Report 0x29 - Beep Control:");
            if (length >= 2) {
                ups_data.beep_control = data[1];
                ESP_LOGI(TAG, "  Beep Control: %d", ups_data.beep_control);
            }
            break;
            
        case 0x80:  // System Status
            ESP_LOGI(TAG, "Report 0x80 - System Status:");
            if (length >= 2) {
                ups_data.system_status = data[1];
                ESP_LOGI(TAG, "  System Status: %d", ups_data.system_status);
            }
            break;
            
        case 0x82:  // Extended Status
            ESP_LOGI(TAG, "Report 0x82 - Extended Status:");
            if (length >= 3) {
                ups_data.extended_status = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Extended Status: %d", ups_data.extended_status);
            }
            break;
            
        case 0x85:  // Temperature/Sensor
            ESP_LOGI(TAG, "Report 0x85 - Temperature/Sensor:");
            if (length >= 2) {
                ups_data.temperature = data[1];
                ESP_LOGI(TAG, "  Temperature: %d", ups_data.temperature);
            }
            break;
            
        case 0x86:  // Temperature Range 1
            ESP_LOGI(TAG, "Report 0x86 - Temperature Range 1:");
            if (length >= 3) {
                ups_data.temp_range1 = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Temp Range 1: %d", ups_data.temp_range1);
            }
            break;
            
        case 0x87:  // Temperature Range 2
            ESP_LOGI(TAG, "Report 0x87 - Temperature Range 2:");
            if (length >= 3) {
                ups_data.temp_range2 = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Temp Range 2: %d", ups_data.temp_range2);
            }
            break;
            
        case 0x88:  // Additional Sensor
            ESP_LOGI(TAG, "Report 0x88 - Additional Sensor:");
            if (length >= 3) {
                ups_data.additional_sensor = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Additional Sensor: %d", ups_data.additional_sensor);
            }
            break;
            
        default:
            ESP_LOGI(TAG, "Report 0x%02X - UNKNOWN REPORT TYPE", report_id);
            ESP_LOGI(TAG, "  Raw data:");
            for (int i = 0; i < length && i < 16; i++) {
                printf("%02X ", data[i]);
            }
            if (length > 16) printf("...");
            printf("\n");
            break;
    }
    
#if VERBOSE_UPS_LOGGING
    ESP_LOGI(TAG, "=============================");
#endif

    // Print current UPS data state after each report
    ESP_LOGI(TAG, "=== CURRENT UPS DATA STATE ===");
    ESP_LOGI(TAG, "State: %d, Available: %s, Last Data: %lu ms ago (timeout: %d ms)", 
             ups_state, ups_available ? "YES" : "NO", 
             xTaskGetTickCount() * portTICK_PERIOD_MS - ups_last_data_time,
             UPS_DATA_FRESHNESS_TIMEOUT_MS);
    ESP_LOGI(TAG, "Battery: %d%%, Load: %d%%, Runtime: %d min", 
             ups_data.battery_level, ups_data.load, ups_data.runtime);
    ESP_LOGI(TAG, "Input: %d V, Output: %d V, Temp: %d", 
             ups_data.input_voltage, ups_data.output_voltage, ups_data.temperature);
    ESP_LOGI(TAG, "Status: %d, System: %d, Extended: %d", 
             ups_data.status, ups_data.system_status, ups_data.extended_status);
    ESP_LOGI(TAG, "=============================");
}

/**
 * @brief USB HID Host interface callback
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host interface event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                 const hid_host_interface_event_t event,
                                 void *arg)
{
    uint8_t data[64] = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event)
    {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle,
                                                                  data,
                                                                  64,
                                                                  &data_length));

        // UPS filtering logic - check if this is the first data from a newly connected device
        if (waiting_for_initial_data && hid_device_handle == current_device_handle) {
            uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t time_since_connection = current_time - device_connection_time;
            
            if (time_since_connection <= UPS_DATA_TIMEOUT_MS) {
                // Device sent data within timeout - likely a UPS
                device_is_ups = true;
                waiting_for_initial_data = false;
                latest_hid_device_handle = hid_device_handle;
                UPS_DEV_CONNECTED = true;
                ups_state = UPS_CONNECTED_WAITING_DATA;
                ESP_LOGI(TAG, "UPS data detected, sending to parsing logic");
                
                ESP_LOGI(TAG, "=== UPS PARSING INITIALIZED ===");
                ESP_LOGI(TAG, "Ready to parse HID reports and extract UPS values");
                ESP_LOGI(TAG, "================================");
            }
        }

        if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class)
        {
        }
        else
        {
            // Only process data if device is confirmed as UPS
            if (device_is_ups && hid_device_handle == current_device_handle) {
            hid_host_generic_report_callback(data, data_length);
            }
        }

        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        // Only handle disconnection for devices we actually opened (NONE protocol devices)
        if (hid_device_handle == current_device_handle) {
            // Don't reset filtering state immediately - let timeout check handle it
            // device_is_ups = false;
            // waiting_for_initial_data = false;
            // current_device_handle = NULL;
        }
        
        if (hid_device_handle == latest_hid_device_handle) {
        UPS_DEV_CONNECTED = false;
        ups_state = UPS_DISCONNECTED;
        ups_available = false;
        update_led_with_pulse();  // Update LED when UPS disconnects
            ESP_LOGI(TAG, "UPS state: -> DISCONNECTED");
        }
        
        ESP_LOGI(TAG, "USB device disconnected correctly");
        ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGI(TAG, "hid_host_interface_callback: HID Device, protocol '%s' TRANSFER_ERROR",
                 hid_proto_name_str[dev_params.proto]);
        break;
    default:
        ESP_LOGE(TAG, "hid_host_interface_callback: HID Device, protocol '%s' Unhandled event",
                 hid_proto_name_str[dev_params.proto]);
        break;
    }
}

/**
 * @brief USB HID Host Device event
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host Device event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                           const hid_host_driver_event_t event,
                           void *arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event)
    {
    case HID_HOST_DRIVER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "=== USB DEVICE CONNECTED ===");
        ESP_LOGI(TAG, "Protocol: %s", hid_proto_name_str[dev_params.proto]);
        ESP_LOGI(TAG, "Subclass: %d", dev_params.sub_class);
        ESP_LOGI(TAG, "Device Handle: %p", hid_device_handle);
        ESP_LOGI(TAG, "================================");

        // USB Management: ALWAYS open and manage ALL devices
        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL
        };

        ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
        if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class)
        {
            ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto)
            {
                ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
            }
        }
        ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));

        // Filtering Logic: Only investigate NONE protocol devices for UPS data
        if (dev_params.proto == HID_PROTOCOL_KEYBOARD || dev_params.proto == HID_PROTOCOL_MOUSE) {
            ESP_LOGI(TAG, "USB device detected, not parsing as not UPS");
        }
        else if (dev_params.proto == HID_PROTOCOL_NONE) {
            ESP_LOGI(TAG, "Potential UPS detected, waiting for raw data");
            
            // Initialize UPS filtering for this device
            device_is_ups = false;
            waiting_for_initial_data = true;
            device_connection_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            current_device_handle = hid_device_handle;
        }
        
        break;
    default:
        break;
    }
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);

    while (true/*gpio_get_level(APP_QUIT_PIN) != 0*/)
    {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        // Release devices once all clients has deregistered
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            usb_host_device_free_all();
            ESP_LOGI(TAG, "USB Event flags: NO_CLIENTS");
        }
        // All devices were removed
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
        {
            ESP_LOGI(TAG, "USB Event flags: ALL_FREE");
        }
    }
    // App Button was pressed, trigger the flag
    user_shutdown = true;
    ESP_LOGI(TAG, "USB shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

void set_beep(bool enabled)
{
    esp_err_t ret = set_beep_generic(latest_hid_device_handle, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set beep status: %s", esp_err_to_name(ret));
    }
}

void __attribute__((unused)) refresh_ups_status_from_hid(bool *beep)
{
    // DISABLED - depends on stored data
    ESP_LOGI(TAG, "refresh_ups_status_from_hid disabled - no data storage in current implementation");
    if (beep) {
        *beep = false;
    }
}

/// @brief to recheck ups status and refresh json object. 
/// @param pvParameters 
void timer_task(void *pvParameters)
{
    // Simple timeout check - no complex queue logic
    while (true)
    {
        // Check for UPS detection timeout
        if (waiting_for_initial_data && current_device_handle != NULL) {
            uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t time_since_connection = current_time - device_connection_time;
            
            if (time_since_connection > UPS_DATA_TIMEOUT_MS) {
                // Device didn't send data within timeout - not a UPS
                ESP_LOGI(TAG, "no raw data detected, no UPS connected");
                
                // Reset filtering state
                device_is_ups = false;
                waiting_for_initial_data = false;
                current_device_handle = NULL;
            }
        }

        // Simple delay instead of queue
        vTaskDelay(pdMS_TO_TICKS(100));  // Check every 100ms
    }
}

/**
 * @brief HID Host main task
 *
 * Creates queue and get new event from the queue
 *
 * @param[in] pvParameters Not used
 */
void hid_host_task(void *pvParameters)
{
    hid_host_event_queue_t evt_queue;
    // Create queue
    hid_host_event_queue = xQueueCreate(20, sizeof(hid_host_event_queue_t));

    // Wait queue
    while (!user_shutdown)
    {
        if (xQueueReceive(hid_host_event_queue, &evt_queue, pdMS_TO_TICKS(50)))
        {
            hid_host_device_event(evt_queue.hid_device_handle,
                                  evt_queue.event,
                                  evt_queue.arg);
        }
    }

    xQueueReset(hid_host_event_queue);
    vQueueDelete(hid_host_event_queue);
    vTaskDelete(NULL);
}

/**
 * @brief HID Host Device callback
 *
 * Puts new HID Device event to the queue
 *
 * @param[in] hid_device_handle HID Device handle
 * @param[in] event             HID Device event
 * @param[in] arg               Not used
 */
void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                              const hid_host_driver_event_t event,
                              void *arg)
{
    const hid_host_event_queue_t evt_queue = {
        .hid_device_handle = hid_device_handle,
        .event = event,
        .arg = arg};
    xQueueSend(hid_host_event_queue, &evt_queue, 0);
}

// ==计时器 Timer

static bool IRAM_ATTR __attribute__((unused)) timer_on_alarm_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_ctx;
    timer_queue_element_t ele = {
        .event_count = edata->alarm_value};
    
    xQueueSendFromISR(queue, &ele, &high_task_awoken);
    return high_task_awoken == pdTRUE;
}
// ==计时器 Timer

static void __attribute__((unused)) configure_led(void)
{
    /* LED strip initialization with the GPIO and pixels number*/
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_PIN,
        .max_leds = 1, // Single RGB LED on board
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    /* Set all LED off to clear all pixels */
    led_strip_clear(led_strip);
    /* Ensure the LED is properly initialized */
    led_strip_refresh(led_strip);
}

// LED Status Functions
static void set_led_green(void)
{
    led_strip_set_pixel(led_strip, 0, 0, 255, 0);  // Green
    led_strip_refresh(led_strip);
}

static void set_led_yellow(void)
{
    led_strip_set_pixel(led_strip, 0, 255, 255, 0);  // Yellow
    led_strip_refresh(led_strip);
}

static void set_led_red(void)
{
    led_strip_set_pixel(led_strip, 0, 255, 0, 0);  // Red
    led_strip_refresh(led_strip);
}

static void set_led_white(void)
{
    led_strip_set_pixel(led_strip, 0, 255, 255, 255);  // White
    led_strip_refresh(led_strip);
}

static void set_led_purple(void)
{
    if (led_strip) {
        esp_err_t ret = led_strip_set_pixel(led_strip, 0, 255, 0, 255);  // Purple
        if (ret == ESP_OK) {
            led_strip_refresh(led_strip);
        } else {
            ESP_LOGE(TAG, "Failed to set LED purple: %s", esp_err_to_name(ret));
        }
    }
}

static void set_led_blue(void)
{
    if (led_strip) {
        esp_err_t ret = led_strip_set_pixel(led_strip, 0, 0, 0, 255);  // Blue
        if (ret == ESP_OK) {
            led_strip_refresh(led_strip);
        } else {
            ESP_LOGE(TAG, "Failed to set LED blue: %s", esp_err_to_name(ret));
        }
    }
}



// WiFi Configuration - Layer 1: Reliable WiFi Connection
#include "wifi_secrets.h" // <-- User must create this file with their WiFi credentials
// #define WIFI_SSID "YOUR_WIFI_SSID"
// #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define WIFI_MAXIMUM_RETRY 5
#define WIFI_RECONNECT_DELAY_MS 5000

// WiFi status tracking
static bool wifi_connected = false;
static int wifi_retry_count = 0;
static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;

// LED Status Functions - RESILIENT VERSION
static void update_led_status(void)
{
    // Safe defaults - assume worst case if variables not initialized
    bool wifi_ok = false;
    bool ups_ok = false;
    
    // Safely check WiFi status
    if (wifi_connected) {
        wifi_ok = true;
    }
    
    // Safely check UPS status
    if (ups_state == UPS_CONNECTED_ACTIVE && ups_available) {
        ups_ok = true;
    }
    
    if (wifi_ok && ups_ok) {
        // Green: All good
        set_led_green();
    } else if (!wifi_ok && !ups_ok) {
        // Red: All fucked up
        set_led_red();
    } else {
        // Yellow: Something wrong (one of them is not OK)
        set_led_yellow();
    }
}

static void update_led_with_pulse(void)
{
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Check if we should show a white pulse
    if (!pulse_in_progress && 
        last_field_update_time > last_pulse_time && 
        (current_time - last_pulse_time) >= PULSE_INTERVAL_MS) {
        
        // Start white pulse
        pulse_in_progress = true;
        last_pulse_time = current_time;
        set_led_white();
        
        // Schedule return to base color after pulse duration
        esp_timer_handle_t pulse_timer;
        esp_timer_create_args_t timer_args = {
            .callback = pulse_timer_callback,
            .arg = NULL,
            .name = "pulse_timer"
        };
        esp_timer_create(&timer_args, &pulse_timer);
        esp_timer_start_once(pulse_timer, PULSE_DURATION_MS * 1000);  // Convert to microseconds
        esp_timer_delete(pulse_timer);
        
        return;
    }
    
    // If no pulse needed, show base status
    if (!pulse_in_progress) {
        update_led_status();
    }
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started, attempting to connect...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        update_led_with_pulse();  // Update LED when WiFi disconnects
        if (wifi_retry_count < WIFI_MAXIMUM_RETRY) {
            ESP_LOGI(TAG, "WiFi disconnected, retrying... (attempt %d/%d)", 
                     wifi_retry_count + 1, WIFI_MAXIMUM_RETRY);
            esp_wifi_connect();
            wifi_retry_count++;
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after %d attempts", WIFI_MAXIMUM_RETRY);
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        update_led_with_pulse();  // Update LED when WiFi connects
    }
}

// Initialize WiFi
static esp_err_t wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");
    
    // Create WiFi event group
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create default netif instance
    esp_netif_create_default_wifi_sta();
    
    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers
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
    
    // Load WiFi credentials from NVS or use hardcoded fallback
    char ssid[32] = {0};
    char password[64] = {0};
    esp_err_t nvs_err = load_wifi_credentials_from_nvs(ssid, password, sizeof(ssid));
    if (nvs_err == ESP_OK) {
        ESP_LOGI(TAG, "Using WiFi credentials from NVS");
    } else {
        // Fall back to hardcoded credentials
        strcpy(ssid, WIFI_SSID);
        strcpy(password, WIFI_PASSWORD);
        ESP_LOGI(TAG, "Using hardcoded WiFi credentials (NVS not available)");
    }
    
    // Configure WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // Copy credentials to wifi_config
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, password);
    
    // Set WiFi mode and config
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi initialization complete");
    return ESP_OK;
}

// Connect to WiFi with timeout
static esp_err_t wifi_connect_with_timeout(int timeout_ms)
{
    // Load current WiFi credentials for logging
    char ssid[32] = {0};
    char password[64] = {0};
    esp_err_t nvs_err = load_wifi_credentials_from_nvs(ssid, password, sizeof(ssid));
    if (nvs_err == ESP_OK) {
        ESP_LOGI(TAG, "Connecting to WiFi: %s (from NVS)", ssid);
    } else {
        ESP_LOGI(TAG, "Connecting to WiFi: %s (hardcoded)", WIFI_SSID);
    }
    
    // Reset retry count
    wifi_retry_count = 0;
    
    // Wait a moment for WiFi stack to stabilize
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Check if WiFi is already connecting
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi already connected to: %s", ap_info.ssid);
        wifi_connected = true;
        return ESP_OK;
    }
    
    // Attempt connection with proper error handling
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "WiFi already connecting, waiting for completion...");
            // Wait a bit and try again
            vTaskDelay(pdMS_TO_TICKS(2000));
            ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "WiFi connection failed: %s", esp_err_to_name(ret));
                return ret;
            }
        } else {
            ESP_LOGE(TAG, "WiFi connection failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed");
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
        return ESP_ERR_TIMEOUT;
    }
}

// Check WiFi connection status


// WiFi reconnection task (runs in background)
static void wifi_reconnect_task(void *pvParameters)
{
    while (1) {
        if (!wifi_connected) {
            ESP_LOGI(TAG, "WiFi disconnected, attempting reconnection...");
            wifi_connect_with_timeout(10000); // 10 second timeout
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS));
    }
}

// Public WiFi connection function
void connect_to_wifi(void)
{
    ESP_LOGI(TAG, "Starting WiFi connection...");
    
    // Initialize WiFi
    esp_err_t ret = wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialization failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Connect to WiFi with retry logic
    int retry_count = 0;
    const int max_retries = 3;
    
    while (retry_count < max_retries) {
        ret = wifi_connect_with_timeout(15000); // 15 second timeout
        if (ret == ESP_OK) {
            break;
        }
        
        retry_count++;
        ESP_LOGW(TAG, "WiFi connection attempt %d failed: %s", retry_count, esp_err_to_name(ret));
        
        if (retry_count < max_retries) {
            ESP_LOGI(TAG, "Retrying WiFi connection in 5 seconds...");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed after %d attempts", max_retries);
        return;
    }
    
    // Start reconnection task
    xTaskCreate(wifi_reconnect_task, "wifi_reconnect", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "WiFi connection established and monitoring started");
    
    // Initial LED status update
    update_led_with_pulse();
}

// Forward declaration for resilience logic
void handle_accept_error(void);
void webserver_restart(void);

// ===================== LOG MONITOR MODULARIZATION =====================
// To enable the log monitor logic, set ENABLE_LOG_MONITOR to 1 below.
// To disable all log monitor logic (as if it never existed), set to 0.
// ======================================================================
#define ENABLE_LOG_MONITOR 0
// ======================================================================
#if ENABLE_LOG_MONITOR
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_SCAN_TASK_STACK 4096
#define LOG_SCAN_TASK_PRIO 2
#define LOG_SCAN_QUEUE_LEN 32
#define LOG_SCAN_LINE_MAX 256

static QueueHandle_t log_scan_queue = NULL;
static int (*orig_vprintf)(const char *, va_list) = NULL;

// Log hook: enqueue log lines for processing in a safe context
static int log_intercept_vprintf(const char *fmt, va_list args) {
    char buf[LOG_SCAN_LINE_MAX];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0 && log_scan_queue) {
        buf[LOG_SCAN_LINE_MAX-1] = '\0';
        char *heap_line = malloc(strlen(buf) + 1);
        if (heap_line) {
            strcpy(heap_line, buf);
            if (xQueueSend(log_scan_queue, &heap_line, 0) != pdTRUE) {
                free(heap_line); // Drop if queue full
            }
        }
    }
    // Call the original vprintf, not vprintf directly, to avoid recursion
    if (orig_vprintf) {
        return orig_vprintf(fmt, args);
    }
    return 0;
}

// Log scanner task: dequeue and process log lines (no logging, no recursion)
static void log_scanner_task(void *arg) {
    char *line = NULL;
    TickType_t last_log = xTaskGetTickCount();
    while (1) {
        if (xQueueReceive(log_scan_queue, &line, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
            // Only process the line (e.g., scan for error strings, trigger logic)
            free(line);
        }
        // Periodically log stack high water mark
        if (xTaskGetTickCount() - last_log > 30000 / portTICK_PERIOD_MS) {
            last_log = xTaskGetTickCount();
            ESP_LOGI("logscan", "Stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
        }
    }
}
#endif // ENABLE_LOG_MONITOR

#define HEAP_CRITICAL_THRESHOLD (16 * 1024) // 16 KB
#define SELF_HTTP_CHECK_INTERVAL_MS 10000
#define SELF_HTTP_CHECK_FAIL_LIMIT 3
#define SELF_HTTP_CHECK_TIMEOUT_MS 2000

// --- Periodic Heap Check Task ---
static void heap_check_task(void *pvParameters) {
    const char *TAG = "heap-check";
    while (1) {
        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "Heap check: %u bytes free", (unsigned)free_heap);
        if (free_heap < HEAP_CRITICAL_THRESHOLD) {
            ESP_LOGE(TAG, "Free heap critically low (%u bytes), rebooting!", (unsigned)free_heap);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // Check every 10s
    }
}

// --- Self-HTTP Health Check Task ---
static void self_http_check_task(void *pvParameters) {
    const char *TAG = "self-http-check";
    int fail_count = 0;
    int check_count = 0;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1/api/wifi_status");
    while (1) {
        check_count++;
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = SELF_HTTP_CHECK_TIMEOUT_MS,
            .disable_auto_redirect = true,
            .method = HTTP_METHOD_GET,
            .buffer_size = 256,
            .buffer_size_tx = 256,
            .is_async = false,
            .keep_alive_enable = false,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            esp_http_client_set_header(client, "Connection", "close");
            esp_err_t err = esp_http_client_perform(client);
            int status = esp_http_client_get_status_code(client);
            if (err == ESP_OK && status == 200) {
                ESP_LOGI(TAG, "Self-HTTP check #%d: SUCCESS (status=%d)", check_count, status);
                fail_count = 0;
            } else {
                fail_count++;
                ESP_LOGW(TAG, "Self-HTTP check #%d: FAIL (status=%d, err=%s, fail_count=%d/%d)", check_count, status, esp_err_to_name(err), fail_count, SELF_HTTP_CHECK_FAIL_LIMIT);
                if (fail_count >= SELF_HTTP_CHECK_FAIL_LIMIT) {
                    ESP_LOGE(TAG, "3 consecutive failures, restarting webserver");
                    webserver_restart();
                    fail_count = 0;
                }
            }
            esp_http_client_cleanup(client);
        } else {
            ESP_LOGE(TAG, "Self-HTTP check #%d: Failed to init HTTP client", check_count);
            fail_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(SELF_HTTP_CHECK_INTERVAL_MS));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Configure BOOT button (GPIO 0) for continuous monitoring
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    ESP_LOGI(TAG, "BOOT button configured for continuous monitoring");
    
    //init_json_object();
    //ESP_ERROR_CHECK(init_generic_ups_models());
    //connect_to_wifi();
    connect_to_wifi();
    
    // Start button monitoring task
    xTaskCreate(button_monitor_task, "button_monitor", 3072, NULL, 5, NULL);
    //SemaphoreHandle_t server_ready = xSemaphoreCreateBinary();
    //assert(server_ready);
    //xTaskCreate(tcp_server_task, "tcp_server", 4096, &server_ready, 5, NULL);
    //xSemaphoreTake(server_ready, portMAX_DELAY);
    //vSemaphoreDelete(server_ready);
    //timer_queue = xQueueCreate(10, sizeof(timer_queue_element_t));
    //gptimer_handle_t gptimer = NULL;
    //gptimer_config_t timer_config = {
    //    .clk_src = GPTIMER_CLK_SRC_DEFAULT,
    //    .direction = GPTIMER_COUNT_UP,
    //    .resolution_hz = 1 * 1000 * 1000, // 1MHz, 1 tick = 1us
    //};
    //ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));
    //gptimer_alarm_config_t alarm_config = {
    //    .reload_count = 0,                  // counter will reload with 0 on alarm event
    //    .alarm_count = 1000000,             // period = 1s @resolution 1MHz
    //    .flags.auto_reload_on_alarm = true, // enable auto-reload
    //};
    //ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));
    //gptimer_event_callbacks_t cbs = {
    //    .on_alarm = timer_on_alarm_callback,
    //};
    //ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, timer_queue));
    //ESP_ERROR_CHECK(gptimer_enable(gptimer));
    //ESP_ERROR_CHECK(gptimer_start(gptimer));
    BaseType_t task_created;
    task_created = xTaskCreatePinnedToCore(usb_lib_task,
                                           "usb_events",
                                           4096,
                                           xTaskGetCurrentTaskHandle(),
                                           2, NULL, 0);
    assert(task_created == pdTRUE);
    ulTaskNotifyTake(false, 1000);
    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL};
    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));
    user_shutdown = false;
    task_created = xTaskCreate(&hid_host_task, "hid_task", 4 * 1024, NULL, 2, NULL);
    configure_led();
    task_created = xTaskCreate(&timer_task, "timer_task", 4 * 1024, NULL, 8, NULL);
    assert(task_created == pdTRUE);
    // Start TCP server for NUT protocol
    task_created = xTaskCreate(&tcp_server_task, "tcp_server", 4096, NULL, 5, &tcp_server_task_handle);
    assert(task_created == pdTRUE);
    
    // Start UPS freshness timer task
    task_created = xTaskCreate(ups_freshness_timer_task, "ups_timer", 3072, NULL, 3, NULL);
    assert(task_created == pdTRUE);
    
    // Start webserver
    esp_err_t ret = webserver_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start webserver: %s", esp_err_to_name(ret));
    }
    
    // Start heap check task
    xTaskCreate(heap_check_task, "heap_check", 4096, NULL, 2, NULL);
    // Start self-HTTP health check task
    xTaskCreate(self_http_check_task, "self_http_check", 3072, NULL, 2, NULL);
}

//static const char *TAG = "wifi";

// TODO: Layer 2 - USB HID implementation will go here
// For now, just a placeholder to avoid compilation errors
static esp_err_t __attribute__((unused)) init_generic_ups_models(void)
{
    ESP_LOGI(TAG, "UPS models initialization - will be implemented in Layer 2");
    return ESP_OK;
}

// REMOVED - unused function
// static esp_err_t detect_ups_model(hid_host_device_handle_t device_handle)
// {
//     // ... function body removed ...
// }

static uint8_t __attribute__((unused)) extract_field_value(const uint8_t *data, const hid_report_mapping_t *mapping)
{
    if (mapping->data_offset + mapping->data_size > MAX_REPORT_SIZE) {
        return 0;
    }
    
    uint8_t value = 0;
    for (int i = 0; i < mapping->data_size; i++) {
        value |= data[mapping->data_offset + i] << (i * 8);
    }
    return value;
}

static uint32_t __attribute__((unused)) extract_multi_byte_value(const uint8_t *data, const hid_report_mapping_t *mapping)
{
    if (mapping->data_offset + mapping->data_size > MAX_REPORT_SIZE) {
        return 0;
    }
    
    uint32_t value = 0;
    for (int i = 0; i < mapping->data_size; i++) {
        value |= (uint32_t)data[mapping->data_offset + i] << (i * 8);
    }
    return value;
}

static esp_err_t __attribute__((unused)) parse_ups_data_generic(hid_host_device_handle_t device_handle, ups_data_t *data)
{
    // DISABLED - depends on stored data
    ESP_LOGI(TAG, "parse_ups_data_generic disabled - no data storage in current implementation");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t set_beep_generic(hid_host_device_handle_t device_handle, bool enabled)
{
    if (!model_detected || detected_model_index >= MAX_UPS_MODELS) {
        ESP_LOGE(TAG, "No UPS model detected");
        return ESP_ERR_INVALID_STATE;
    }
    
    ups_model_config_t *model = &ups_models[detected_model_index];
    uint8_t send[2] = {model->beep_report_id, enabled ? model->beep_enable_value : model->beep_disable_value};
    size_t len = 2;
    
    return hid_class_request_set_report(device_handle, 0x03, model->beep_report_id, send, len);
}

static void __attribute__((unused)) update_json_with_ups_data(const ups_data_t *data)
{
    // DISABLED - depends on stored data
    ESP_LOGI(TAG, "update_json_with_ups_data disabled - no data storage in current implementation");
}



static void __attribute__((unused)) debug_unknown_ups_model(hid_host_device_handle_t device_handle)  // DISABLED - depends on stored data
{
    // DISABLED - depends on stored data
    ESP_LOGI(TAG, "debug_unknown_ups_model disabled - no data storage in current implementation");
}

// Load WiFi credentials from NVS
static esp_err_t load_wifi_credentials_from_nvs(char* ssid, char* password, size_t max_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS handle for WiFi config: %s", esp_err_to_name(err));
        return err;
    }
    
    size_t ssid_len = max_len;
    size_t pass_len = max_len;
    
    err = nvs_get_str(nvs_handle, WIFI_SSID_KEY, ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read SSID from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_get_str(nvs_handle, WIFI_PASS_KEY, password, &pass_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read password from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded WiFi credentials from NVS: SSID=%s", ssid);
    return ESP_OK;
}

// Button monitoring task for WiFi reset
static void button_monitor_task(void* pvParameters)
{
    const int BUTTON_HOLD_TIME_MS = 5000;  // 5 seconds
    const int CHECK_INTERVAL_MS = 100;     // Check every 100ms
    
    TickType_t button_press_start = 0;
    bool button_was_pressed = false;
    bool reset_triggered = false;
    
    ESP_LOGI(TAG, "Button monitoring task started");
    
    while (1) {
        // Check if BOOT button is pressed (LOW = pressed due to pullup)
        bool button_pressed = (gpio_get_level(GPIO_NUM_0) == 0);
        
        if (button_pressed && !button_was_pressed) {
            // Button just pressed - start timing
            button_press_start = xTaskGetTickCount();
            button_was_pressed = true;
            ESP_LOGI(TAG, "BOOT button pressed - start monitoring hold time");
        }
        else if (button_pressed && button_was_pressed && !reset_triggered) {
            // Button still held - check if we've reached the hold time
            TickType_t current_time = xTaskGetTickCount();
            TickType_t hold_duration = (current_time - button_press_start) * portTICK_PERIOD_MS;
            
            if (hold_duration >= BUTTON_HOLD_TIME_MS) {
                // Button held for 5+ seconds - trigger reset
                reset_triggered = true;
                ESP_LOGI(TAG, "BOOT button held for 5 seconds - clearing WiFi credentials and rebooting");
                
                // Turn LED blue to indicate it's safe to release button
                set_led_blue();
                
                // Clear WiFi credentials from NVS
                nvs_handle_t nvs_handle;
                esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
                if (err == ESP_OK) {
                    nvs_erase_key(nvs_handle, WIFI_SSID_KEY);
                    nvs_erase_key(nvs_handle, WIFI_PASS_KEY);
                    nvs_commit(nvs_handle);
                    nvs_close(nvs_handle);
                    ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
                } else {
                    ESP_LOGW(TAG, "Failed to clear WiFi credentials: %s", esp_err_to_name(err));
                }
                
                // Wait 2 seconds with blue LED to give user time to release button
                vTaskDelay(pdMS_TO_TICKS(2000));
                
                // Reboot
                esp_restart();
            }
            else if (hold_duration >= 1000) {
                // Flash purple LED after 1 second to show button is being monitored
                static TickType_t last_flash = 0;
                TickType_t current_flash = xTaskGetTickCount();
                if ((current_flash - last_flash) * portTICK_PERIOD_MS >= 500) {
                    set_led_purple();
                    last_flash = current_flash;
                }
            }
        }
        else if (!button_pressed && button_was_pressed) {
            // Button released - reset state
            button_was_pressed = false;
            reset_triggered = false;
            ESP_LOGI(TAG, "BOOT button released");
        }
        
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

// Getter for UPS state
ups_connection_state_t get_ups_state(void) {
    return ups_state;
}
// Getter for last UPS data time
unsigned int get_ups_last_data_time(void) {
    return ups_last_data_time;
}

// Add a stub for webserver_restart if not present
void __attribute__((weak)) webserver_restart(void) {
    ESP_LOGW("webserver", "Restarting webserver due to health check failure");
    // ... existing restart logic ...
}



