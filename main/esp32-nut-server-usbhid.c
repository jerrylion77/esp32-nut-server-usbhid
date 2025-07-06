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
#include "protocol_examples_common.h"

#include "led_strip.h"

#include "cJSON.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "ups_models_config.h"

#include <inttypes.h>

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

// TCP Server Task - DISABLED
static void __attribute__((unused)) tcp_server_task(void *pvParameters)
{
    // DISABLED - depends on stored data
    ESP_LOGI(TAG, "TCP server task disabled - no data storage in current implementation");
    vTaskDelete(NULL);
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
                ESP_LOGI(TAG, "  Battery Level: %d%%", data[1]);
            }
            if (length >= 3) {
                ESP_LOGI(TAG, "  Battery Byte2: %d", data[2]);
            }
            if (length >= 4) {
                ESP_LOGI(TAG, "  Battery Byte3: %d", data[3]);
            }
            break;
            
        case 0x21:  // Status Flags
            ESP_LOGI(TAG, "Report 0x21 - Status Flags:");
            if (length >= 2) {
                ESP_LOGI(TAG, "  Status: %d", data[1]);
            }
            if (length >= 3) {
                ESP_LOGI(TAG, "  Status Byte2: %d", data[2]);
            }
            break;
            
        case 0x22:  // Runtime
            ESP_LOGI(TAG, "Report 0x22 - Runtime Data:");
            if (length >= 2) {
                ESP_LOGI(TAG, "  Runtime: %d minutes", data[1]);
            }
            break;
            
        case 0x23:  // Voltage Data
            ESP_LOGI(TAG, "Report 0x23 - Voltage Data:");
            if (length >= 3) {
                uint16_t voltage1 = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Input Voltage: %d V", voltage1);
            }
            if (length >= 5) {
                uint16_t voltage2 = (data[4] << 8) | data[3];
                ESP_LOGI(TAG, "  Output Voltage: %d V", voltage2);
            }
            break;
            
        case 0x25:  // Load Percentage
            ESP_LOGI(TAG, "Report 0x25 - Load Data:");
            if (length >= 2) {
                ESP_LOGI(TAG, "  Load: %d%%", data[1]);
            }
            break;
            
        case 0x28:  // Alarm Control
            ESP_LOGI(TAG, "Report 0x28 - Alarm Control:");
            if (length >= 2) {
                ESP_LOGI(TAG, "  Alarm Control: %d", data[1]);
            }
            break;
            
        case 0x29:  // Beep Control
            ESP_LOGI(TAG, "Report 0x29 - Beep Control:");
            if (length >= 2) {
                ESP_LOGI(TAG, "  Beep Control: %d", data[1]);
            }
            break;
            
        case 0x80:  // System Status
            ESP_LOGI(TAG, "Report 0x80 - System Status:");
            if (length >= 2) {
                ESP_LOGI(TAG, "  System Status: %d", data[1]);
            }
            break;
            
        case 0x82:  // Extended Status
            ESP_LOGI(TAG, "Report 0x82 - Extended Status:");
            if (length >= 3) {
                uint16_t ext_status = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Extended Status: %d", ext_status);
            }
            break;
            
        case 0x85:  // Temperature/Sensor
            ESP_LOGI(TAG, "Report 0x85 - Temperature/Sensor:");
            if (length >= 2) {
                ESP_LOGI(TAG, "  Temperature: %d", data[1]);
            }
            break;
            
        case 0x86:  // Temperature Range 1
            ESP_LOGI(TAG, "Report 0x86 - Temperature Range 1:");
            if (length >= 3) {
                uint16_t temp_range = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Temp Range 1: %d", temp_range);
            }
            break;
            
        case 0x87:  // Temperature Range 2
            ESP_LOGI(TAG, "Report 0x87 - Temperature Range 2:");
            if (length >= 3) {
                uint16_t temp_range = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Temp Range 2: %d", temp_range);
            }
            break;
            
        case 0x88:  // Additional Sensor
            ESP_LOGI(TAG, "Report 0x88 - Additional Sensor:");
            if (length >= 3) {
                uint16_t sensor_value = (data[2] << 8) | data[1];
                ESP_LOGI(TAG, "  Additional Sensor: %d", sensor_value);
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
        .max_leds = 1, // at least one LED on board
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    /* Set all LED off to clear all pixels */
    led_strip_clear(led_strip);
}

// WiFi Configuration - Layer 1: Reliable WiFi Connection
#define WIFI_SSID "OsoNet"
#define WIFI_PASSWORD "180219771802"
#define WIFI_MAXIMUM_RETRY 5
#define WIFI_RECONNECT_DELAY_MS 5000

// WiFi status tracking
static bool wifi_connected = false;
static int wifi_retry_count = 0;
static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started, attempting to connect...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
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
    
    // Configure WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
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
    ESP_LOGI(TAG, "Connecting to WiFi: %s", WIFI_SSID);
    
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
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    //init_json_object();
    //ESP_ERROR_CHECK(init_generic_ups_models());
    //connect_to_wifi();
    connect_to_wifi();
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
    //configure_led();
    task_created = xTaskCreate(&timer_task, "timer_task", 4 * 1024, NULL, 8, NULL);
    assert(task_created == pdTRUE);
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



