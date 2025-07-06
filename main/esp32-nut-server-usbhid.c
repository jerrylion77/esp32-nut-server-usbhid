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

// Global variable to hold the latest UPS data for NUT reporting
static ups_data_t latest_ups_data = {0};

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

// Store debug data for CyberPower UPS
static uint8_t debug_report_data[16][MAX_REPORT_SIZE];
static size_t debug_report_lengths[16];
static bool debug_data_available = false;

// Store live HID data for UPS parsing
static uint8_t live_hid_data[256][MAX_REPORT_SIZE];  // Support all possible report IDs (0x00-0xFF)
static size_t live_hid_lengths[256];
static bool live_data_available = false;
static uint32_t last_live_data_time = 0;

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
static int try_receive(const char *tag, const int sock, char * data, size_t max_len)
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

static void __attribute__((unused)) tcp_server_task(void *pvParameters)
{
    static char rx_buffer[128];
    static const char *TAG = "tcp-svr";
    SemaphoreHandle_t *server_ready = pvParameters;
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM };
    struct addrinfo *address_info;
    int listen_sock = INVALID_SOCK;
    const size_t max_socks = CONFIG_LWIP_MAX_SOCKETS - 1;
    static int sock[CONFIG_LWIP_MAX_SOCKETS - 1];

    // Prepare a list of file descriptors to hold client's sockets, mark all of them as invalid, i.e. available
    for (int i=0; i<max_socks; ++i) {
        sock[i] = INVALID_SOCK;
    }

    // Translating the hostname or a string representation of an IP to address_info
    int res = getaddrinfo(CONFIG_EXAMPLE_TCP_SERVER_BIND_ADDRESS, CONFIG_EXAMPLE_TCP_SERVER_BIND_PORT, &hints, &address_info);
    if (res != 0 || address_info == NULL) {
        ESP_LOGE(TAG, "couldn't get hostname for `%s` "
                      "getaddrinfo() returns %d, addrinfo=%p", CONFIG_EXAMPLE_TCP_SERVER_BIND_ADDRESS, res, address_info);
        goto error;
    }

    // Creating a listener socket
    listen_sock = socket(address_info->ai_family, address_info->ai_socktype, address_info->ai_protocol);

    if (listen_sock < 0) {
        log_socket_error(TAG, listen_sock, errno, "Unable to create socket");
        goto error;
    }
    ESP_LOGI(TAG, "Listener socket created");

    // Marking the socket as non-blocking
    int flags = fcntl(listen_sock, F_GETFL);
    if (fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_socket_error(TAG, listen_sock, errno, "Unable to set socket non blocking");
        goto error;
    }
    //ESP_LOGI(TAG, "Socket marked as non blocking");

    // Binding socket to the given address
    int err = bind(listen_sock, address_info->ai_addr, address_info->ai_addrlen);
    if (err != 0) {
        log_socket_error(TAG, listen_sock, errno, "Socket unable to bind");
        goto error;
    }
    ESP_LOGI(TAG, "Socket bound on %s:%s", CONFIG_EXAMPLE_TCP_SERVER_BIND_ADDRESS, CONFIG_EXAMPLE_TCP_SERVER_BIND_PORT);

    // Set queue (backlog) of pending connections to one (can be more)
    err = listen(listen_sock, 1);
    if (err != 0) {
        log_socket_error(TAG, listen_sock, errno, "Error occurred during listen");
        goto error;
    }
    ESP_LOGI(TAG, "Socket listening");
    xSemaphoreGive(*server_ready);

    // Main loop for accepting new connections and serving all connected clients
    while (1) {
        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);

        // Find a free socket
        int new_sock_index = 0;
        for (new_sock_index=0; new_sock_index<max_socks; ++new_sock_index) {
            if (sock[new_sock_index] == INVALID_SOCK) {
                break;
            }
        }

        // We accept a new connection only if we have a free socket
        if (new_sock_index < max_socks) {
            // Try to accept a new connections
            sock[new_sock_index] = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);

            if (sock[new_sock_index] < 0) {
                if (errno == EWOULDBLOCK) { // The listener socket did not accepts any connection
                                            // continue to serve open connections and try to accept again upon the next iteration
                    ESP_LOGV(TAG, "No pending connections...");
                } else {
                    log_socket_error(TAG, listen_sock, errno, "Error when accepting connection");
                    goto error;
                }
            } else {
                // We have a new client connected -> print it's address
                ESP_LOGI(TAG, "[sock=%d]: Connection accepted from IP:%s", sock[new_sock_index], get_clients_address(&source_addr));

                led_strip_set_pixel(led_strip, 0, 0x01, 0x01, 0x01);
                /* Refresh the strip to send data */
                led_strip_refresh(led_strip);

                // ...and set the client's socket non-blocking
                flags = fcntl(sock[new_sock_index], F_GETFL);
                if (fcntl(sock[new_sock_index], F_SETFL, flags | O_NONBLOCK) == -1) {
                    log_socket_error(TAG, sock[new_sock_index], errno, "Unable to set socket non blocking");
                    goto error;
                }
                //ESP_LOGI(TAG, "[sock=%d]: Socket marked as non blocking", sock[new_sock_index]);
            }
        }

        // We serve all the connected clients in this loop
        for (int i=0; i<max_socks; ++i) {
            if (sock[i] != INVALID_SOCK) {

                // This is an open socket -> try to serve it
                int len = try_receive(TAG, sock[i], rx_buffer, sizeof(rx_buffer));
                if (len < 0) {
                    // Error occurred within this client's socket -> close and mark invalid
                    ESP_LOGI(TAG, "[sock=%d]: try_receive() returned %d -> closing the socket", sock[i], len);
                    close(sock[i]);
                    sock[i] = INVALID_SOCK;
                } else if (len > 0) {
                    ESP_LOGI(TAG, "[NUT] RX from HA: %.*s", len, rx_buffer);
                    if (len > 1 && rx_buffer[len-1] == '\n')
                    {
                        ESP_LOGI(TAG, "[sock=%d]: Received %.*s", sock[i], len-1, rx_buffer);
                    }
                    else
                    {
                        ESP_LOGI(TAG, "[sock=%d]: Received %.*s", sock[i], len, rx_buffer);
                    }
                    
                    // Debug: Print raw buffer contents with hex values
                    ESP_LOGI(TAG, "[sock=%d]: Raw buffer (len=%d): [%s]", sock[i], len, rx_buffer);
                    ESP_LOGI(TAG, "[sock=%d]: Hex dump:", sock[i]);
                    for (int j = 0; j < len && j < 32; j++) {
                        printf("%02x ", (unsigned char)rx_buffer[j]);
                    }
                    printf("\n");
                    
                    const char* rt = NULL;
                    
                    if (UPS_DEV_CONNECTED)
                    {
                        ESP_LOGI(TAG, "[sock=%d]: UPS connected, handling commands", sock[i]);
                        if (str_startswith(rx_buffer, "USERNAME") || str_startswith(rx_buffer, "PASSWORD") || str_startswith(rx_buffer, "LOGIN"))
                        {
                            // Safety Attention: will not check whether the user & password is correct.
                            // Make sure your LAN environment is safe.
                            rt = "OK\n";
                        }
                        else if (str_startswith(rx_buffer, "LIST VAR"))
                        {
                            // Use latest UPS data for dynamic response
                            static char response_buffer[2048];
                            const char *alias = "VP700ELCD";
                            
                            // Get fresh UPS data before responding
                            bool beep = false;
                            refresh_ups_status_from_hid(&beep);
                            
                            // Build status string with proper NUT format
                            char status_str[32] = "";
                            if (latest_ups_data.ac_present) {
                                strcpy(status_str, "OL");
                                if (latest_ups_data.shutdown_imminent) {
                                    strcat(status_str, " LB");
                                }
                                if (!latest_ups_data.good || latest_ups_data.internal_failure || latest_ups_data.need_replacement) {
                                    strcat(status_str, " RB");
                                }
                                if (latest_ups_data.overload) {
                                    strcat(status_str, " OVER");
                                }
                            } else {
                                strcpy(status_str, "OB");
                            }
                            
                            snprintf(response_buffer, sizeof(response_buffer),
                                "BEGIN LIST VAR %s\n"
                                "VAR %s ups.status \"%s\"\n"
                                "VAR %s ups.load \"%d\"\n"
                                "VAR %s ups.battery.charge \"%d\"\n"
                                "VAR %s ups.battery.runtime \"%lu\"\n"
                                "VAR %s ups.battery.voltage \"%.1f\"\n"
                                "VAR %s ups.battery.type \"PbAc\"\n"
                                "VAR %s ups.battery.charge.low \"20\"\n"
                                "VAR %s ups.battery.charger.status \"%s\"\n"
                                "VAR %s ups.input.voltage \"%.1f\"\n"
                                "VAR %s ups.input.frequency \"60.0\"\n"
                                "VAR %s ups.input.frequency.nominal \"60\"\n"
                                "VAR %s ups.output.voltage \"%.1f\"\n"
                                "VAR %s ups.output.frequency \"60.0\"\n"
                                "VAR %s ups.output.frequency.nominal \"60\"\n"
                                "VAR %s ups.power.nominal \"700\"\n"
                                "VAR %s ups.mfr \"CyberPower\"\n"
                                "VAR %s ups.model \"VP700ELCD\"\n"
                                "VAR %s ups.serial \"Unknown\"\n"
                                "VAR %s ups.firmware \"Unknown\"\n"
                                "VAR %s ups.type \"offline / line interactive\"\n"
                                "VAR %s ups.beeper.status \"%s\"\n"
                                "VAR %s ups.delay.shutdown \"20\"\n"
                                "VAR %s ups.delay.start \"30\"\n"
                                "VAR %s ups.timer.shutdown \"0\"\n"
                                "VAR %s ups.timer.start \"0\"\n"
                                "VAR %s ups.battery.charge.low \"20\"\n"
                                "VAR %s ups.battery.type \"PbAc\"\n"
                                "VAR %s ups.input.frequency.nominal \"60\"\n"
                                "VAR %s ups.output.frequency.nominal \"60\"\n"
                                "VAR %s ups.power.nominal \"700\"\n"
                                "VAR %s ups.mfr \"CyberPower\"\n"
                                "VAR %s ups.model \"VP700ELCD\"\n"
                                "VAR %s ups.serial \"Unknown\"\n"
                                "VAR %s ups.firmware \"Unknown\"\n"
                                "VAR %s ups.type \"offline / line interactive\"\n"
                                "VAR %s ups.delay.shutdown \"20\"\n"
                                "VAR %s ups.delay.start \"30\"\n"
                                "END LIST VAR %s\n",
                                alias, // BEGIN LIST VAR %s
                                alias, // VAR %s ups.status
                                status_str, // status string with proper NUT format
                                alias, // VAR %s ups.load
                                latest_ups_data.ups_load, // load int
                                alias, // VAR %s ups.battery.charge
                                latest_ups_data.battery_charge, // charge int
                                alias, // VAR %s ups.battery.runtime
                                (unsigned long)latest_ups_data.battery_runtime, // runtime ulong
                                alias, // VAR %s ups.battery.voltage
                                (float)latest_ups_data.actual_voltage / 10.0f, // voltage float
                                alias, // VAR %s ups.battery.type
                                alias, // VAR %s ups.battery.charge.low
                                alias, // VAR %s ups.battery.charger.status
                                latest_ups_data.charging ? "charging" : (latest_ups_data.discharging ? "discharging" : "floating"), // charger status string
                                alias, // VAR %s ups.input.voltage
                                (float)latest_ups_data.actual_voltage / 10.0f, // input voltage from live data
                                alias, // VAR %s ups.input.frequency
                                alias, // VAR %s ups.input.frequency.nominal
                                alias, // VAR %s ups.output.voltage
                                (float)latest_ups_data.actual_voltage / 10.0f, // output voltage from live data
                                alias, // VAR %s ups.output.frequency
                                alias, // VAR %s ups.output.frequency.nominal
                                alias, // VAR %s ups.power.nominal
                                alias, // VAR %s ups.mfr
                                alias, // VAR %s ups.model
                                alias, // VAR %s ups.serial
                                alias, // VAR %s ups.firmware
                                alias, // VAR %s ups.type
                                alias, // VAR %s ups.beeper.status
                                latest_ups_data.beep_enabled ? "enabled" : "disabled", // beeper status string
                                alias, // VAR %s ups.delay.shutdown
                                alias, // VAR %s ups.delay.start
                                alias, // VAR %s ups.timer.shutdown
                                alias, // VAR %s ups.timer.start
                                alias, // VAR %s ups.battery.charge.low
                                alias, // VAR %s ups.battery.type
                                alias, // VAR %s ups.input.frequency.nominal
                                alias, // VAR %s ups.output.frequency.nominal
                                alias, // VAR %s ups.power.nominal
                                alias, // VAR %s ups.mfr
                                alias, // VAR %s ups.model
                                alias, // VAR %s ups.serial
                                alias, // VAR %s ups.firmware
                                alias, // VAR %s ups.type
                                alias, // VAR %s ups.delay.shutdown
                                alias, // VAR %s ups.delay.start
                                alias  // END LIST VAR %s
                            );
                            rt = response_buffer;
                            ESP_LOGI(TAG, "[sock=%d]: Responding to LIST VAR with live values - Status: %s, Load: %d%%, Battery: %d%%, Runtime: %lu s", 
                                   sock[i], status_str, latest_ups_data.ups_load, latest_ups_data.battery_charge, (unsigned long)latest_ups_data.battery_runtime);
                        }
                        else if (str_startswith(rx_buffer, "LIST UPS"))
                        {
                            // Strictly respond to LIST UPS
                            rt = "BEGIN LIST UPS\nUPS VP700ELCD \"CyberPower VP700ELCD\"\nUPS cyberpower \"CyberPower VP700ELCD\"\nEND LIST UPS\n";
                            ESP_LOGI(TAG, "[sock=%d]: Responding to LIST UPS: %s", sock[i], rt);
                        }
                        else if (str_startswith(rx_buffer, "LIST CMD"))
                        {
                            // Handle LIST CMD command - return empty list since we don't support commands
                            rt = "BEGIN LIST CMD VP700ELCD\nEND LIST CMD VP700ELCD\n";
                            ESP_LOGI(TAG, "[sock=%d]: Responding to LIST CMD VP700ELCD", sock[i]);
                        }
                        else if (str_startswith(rx_buffer, "LIST VAR "))
                        {
                            // Only respond to LIST VAR VP700ELCD
                            char *alias_ptr = rx_buffer + strlen("LIST VAR ");
                            // Remove trailing newline/whitespace
                            char alias[32] = {0};
                            int j = 0;
                            while (*alias_ptr && *alias_ptr != '\r' && *alias_ptr != '\n' && j < 31) {
                                alias[j++] = *alias_ptr++;
                            }
                            alias[j] = '\0';
                            if (strcmp(alias, "VP700ELCD") == 0 || strcmp(alias, "cyberpower") == 0) {
                                // Get fresh UPS data before responding
                                bool beep = false;
                                refresh_ups_status_from_hid(&beep);
                                
                                // Build status string with proper NUT format
                                char status_str[32] = "";
                                if (latest_ups_data.ac_present) {
                                    strcpy(status_str, "OL");
                                    if (latest_ups_data.shutdown_imminent) {
                                        strcat(status_str, " LB");
                                    }
                                    if (!latest_ups_data.good || latest_ups_data.internal_failure || latest_ups_data.need_replacement) {
                                        strcat(status_str, " RB");
                                    }
                                    if (latest_ups_data.overload) {
                                        strcat(status_str, " OVER");
                                    }
                                } else {
                                    strcpy(status_str, "OB");
                                }
                                
                                // Use the requested alias in the response
                                static char response_buffer[2048];
                                snprintf(response_buffer, sizeof(response_buffer),
                                    "BEGIN LIST VAR %s\n"
                                    "VAR %s ups.status \"%s\"\n"
                                    "VAR %s ups.load \"%d\"\n"
                                    "VAR %s ups.battery.charge \"%d\"\n"
                                    "VAR %s ups.battery.runtime \"%lu\"\n"
                                    "VAR %s ups.battery.voltage \"%.1f\"\n"
                                    "VAR %s ups.battery.type \"PbAc\"\n"
                                    "VAR %s ups.battery.charge.low \"20\"\n"
                                    "VAR %s ups.battery.charger.status \"%s\"\n"
                                    "VAR %s ups.input.voltage \"%.1f\"\n"
                                    "VAR %s ups.input.frequency \"60.0\"\n"
                                    "VAR %s ups.input.frequency.nominal \"60\"\n"
                                    "VAR %s ups.output.voltage \"%.1f\"\n"
                                    "VAR %s ups.output.frequency \"60.0\"\n"
                                    "VAR %s ups.output.frequency.nominal \"60\"\n"
                                    "VAR %s ups.power.nominal \"700\"\n"
                                    "VAR %s ups.mfr \"CyberPower\"\n"
                                    "VAR %s ups.model \"VP700ELCD\"\n"
                                    "VAR %s ups.serial \"Unknown\"\n"
                                    "VAR %s ups.firmware \"Unknown\"\n"
                                    "VAR %s ups.type \"offline / line interactive\"\n"
                                    "VAR %s ups.beeper.status \"%s\"\n"
                                    "VAR %s ups.delay.shutdown \"20\"\n"
                                    "VAR %s ups.delay.start \"30\"\n"
                                    "VAR %s ups.timer.shutdown \"0\"\n"
                                    "VAR %s ups.timer.start \"0\"\n"
                                    "END LIST VAR %s\n",
                                    alias, // BEGIN LIST VAR %s
                                    alias, // VAR %s ups.status
                                    status_str, // status string with proper NUT format
                                    alias, // VAR %s ups.load
                                    latest_ups_data.ups_load, // load int
                                    alias, // VAR %s ups.battery.charge
                                    latest_ups_data.battery_charge, // charge int
                                    alias, // VAR %s ups.battery.runtime
                                    (unsigned long)latest_ups_data.battery_runtime, // runtime ulong
                                    alias, // VAR %s ups.battery.voltage
                                    (float)latest_ups_data.actual_voltage / 10.0f, // voltage float
                                    alias, // VAR %s ups.battery.type
                                    alias, // VAR %s ups.battery.charge.low
                                    alias, // VAR %s ups.battery.charger.status
                                    latest_ups_data.charging ? "charging" : (latest_ups_data.discharging ? "discharging" : "floating"), // charger status string
                                    alias, // VAR %s ups.input.voltage
                                    (float)latest_ups_data.actual_voltage / 10.0f, // input voltage from live data
                                    alias, // VAR %s ups.input.frequency
                                    alias, // VAR %s ups.input.frequency.nominal
                                    alias, // VAR %s ups.output.voltage
                                    (float)latest_ups_data.actual_voltage / 10.0f, // output voltage from live data
                                    alias, // VAR %s ups.output.frequency
                                    alias, // VAR %s ups.output.frequency.nominal
                                    alias, // VAR %s ups.power.nominal
                                    alias, // VAR %s ups.mfr
                                    alias, // VAR %s ups.model
                                    alias, // VAR %s ups.serial
                                    alias, // VAR %s ups.firmware
                                    alias, // VAR %s ups.type
                                    alias, // VAR %s ups.beeper.status
                                    latest_ups_data.beep_enabled ? "enabled" : "disabled", // beeper status string
                                    alias, // VAR %s ups.delay.shutdown
                                    alias, // VAR %s ups.delay.start
                                    alias, // VAR %s ups.timer.shutdown
                                    alias, // VAR %s ups.timer.start
                                    alias  // END LIST VAR %s
                                );
                                rt = response_buffer;
                                ESP_LOGI(TAG, "[sock=%d]: Responding to LIST VAR %s with live values - Status: %s, Load: %d%%, Battery: %d%%, Runtime: %lu s", 
                                       sock[i], alias, status_str, latest_ups_data.ups_load, latest_ups_data.battery_charge, (unsigned long)latest_ups_data.battery_runtime);
                            } else {
                                rt = "ERR UNKNOWN UPS\n";
                                ESP_LOGI(TAG, "[sock=%d]: Responding to LIST VAR for unknown alias: %s", sock[i], alias);
                            }
                        }
                        else if (str_startswith(rx_buffer, "GET VAR VP700ELCD "))
                        {
                            // Get fresh UPS data before responding
                            bool beep = false;
                            refresh_ups_status_from_hid(&beep);
                            
                            // Build status string with proper NUT format
                            char status_str[32] = "";
                            if (latest_ups_data.ac_present) {
                                strcpy(status_str, "OL");
                                if (latest_ups_data.shutdown_imminent) {
                                    strcat(status_str, " LB");
                                }
                                if (!latest_ups_data.good || latest_ups_data.internal_failure || latest_ups_data.need_replacement) {
                                    strcat(status_str, " RB");
                                }
                                if (latest_ups_data.overload) {
                                    strcat(status_str, " OVER");
                                }
                            } else {
                                strcpy(status_str, "OB");
                            }
                            
                            // Handle GET VAR for specific variables
                            if (strstr(rx_buffer, "ups.status") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.status \"%s\"\n", status_str);
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.load") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.load \"%d\"\n", latest_ups_data.ups_load);
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.battery.charge") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.battery.charge \"%d\"\n", latest_ups_data.battery_charge);
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.battery.runtime") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.battery.runtime \"%lu\"\n", (unsigned long)latest_ups_data.battery_runtime);
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.battery.charger.status") != NULL) {
                                static char response_buffer[128];
                                const char *charger_status = latest_ups_data.charging ? "charging" : (latest_ups_data.discharging ? "discharging" : "floating");
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.battery.charger.status \"%s\"\n", charger_status);
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.battery.voltage") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.battery.voltage \"%.1f\"\n", (float)latest_ups_data.actual_voltage / 10.0f);
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.input.voltage") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.input.voltage \"%.1f\"\n", (float)latest_ups_data.actual_voltage / 10.0f);
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.output.voltage") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.output.voltage \"%.1f\"\n", (float)latest_ups_data.actual_voltage / 10.0f);
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.input.frequency") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.input.frequency \"60.0\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.output.frequency") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.output.frequency \"60.0\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.power.nominal") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.power.nominal \"700\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.mfr") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.mfr \"CyberPower\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.model") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.model \"VP700ELCD\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.beeper.status") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.beeper.status \"%s\"\n", latest_ups_data.beep_enabled ? "enabled" : "disabled");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.battery.charge.low") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.battery.charge.low \"20\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.battery.type") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.battery.type \"PbAc\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.input.frequency.nominal") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.input.frequency.nominal \"60\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.output.frequency.nominal") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.output.frequency.nominal \"60\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.delay.shutdown") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.delay.shutdown \"20\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.delay.start") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.delay.start \"30\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.timer.shutdown") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.timer.shutdown \"0\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.timer.start") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.timer.start \"0\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.serial") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.serial \"Unknown\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.firmware") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.firmware \"Unknown\"\n");
                                rt = response_buffer;
                            } else if (strstr(rx_buffer, "ups.type") != NULL) {
                                static char response_buffer[128];
                                snprintf(response_buffer, sizeof(response_buffer), "VAR VP700ELCD ups.type \"offline / line interactive\"\n");
                                rt = response_buffer;
                            } else {
                                rt = "ERR UNKNOWN VARIABLE\n";
                            }
                            ESP_LOGI(TAG, "[sock=%d]: Responding to GET VAR with live value", sock[i]);
                        }
                        else if (str_startswith(rx_buffer, "LOGOUT"))
                        {
                            char *bye_text = "OK Goodbye\n";
                            rt = bye_text;
                        }
                        else {
                            rt = "ERR UNKNOWN COMMAND\n";
                        }
                    }
                    else
                    {
                        ESP_LOGI(TAG, "[sock=%d]: No UPS connected, handling basic NUT commands", sock[i]);
                        // UPS not connected, only handle authentication commands
                        if (str_startswith(rx_buffer, "USERNAME") || str_startswith(rx_buffer, "PASSWORD") || str_startswith(rx_buffer, "LOGIN"))
                        {
                            rt = "OK\n";
                        }
                        else if (strncasecmp(rx_buffer, "LIST UPS", 8) == 0) {
                            // Return empty list when no UPS is connected
                            rt = "BEGIN LIST UPS\nEND LIST UPS\n";
                            ESP_LOGI(TAG, "[sock=%d]: No UPS connected, returning empty LIST UPS", sock[i]);
                        }
                        else if (str_startswith(rx_buffer, "LIST CMD"))
                        {
                            // Return empty list when no UPS is connected
                            rt = "BEGIN LIST CMD\nEND LIST CMD\n";
                            ESP_LOGI(TAG, "[sock=%d]: No UPS connected, returning empty LIST CMD", sock[i]);
                        }
                        else if (str_startswith(rx_buffer, "LIST VAR"))
                        {
                            // Return empty list when no UPS is connected
                            rt = "BEGIN LIST VAR\nEND LIST VAR\n";
                            ESP_LOGI(TAG, "[sock=%d]: No UPS connected, returning empty LIST VAR", sock[i]);
                        }
                        else {
                            rt = "ERR UNKNOWN COMMAND\n";
                        }
                    }

                    if (rt != NULL)
                    {
                        int sent = send(sock[i], rt, strlen(rt), 0);
                        ESP_LOGI(TAG, "[sock=%d]: Sent response (%d bytes): %s", sock[i], sent, rt);
                        if (sent < 0)
                        {
                            ESP_LOGE(TAG, "[sock=%d]: Failed to send response: %s", sock[i], strerror(errno));
                        // Error occurred on write to this socket -> close it and mark invalid
                            ESP_LOGI(TAG, "[sock=%d]: socket_send() returned %d -> closing the socket", sock[i], sent);
                        close(sock[i]);
                        sock[i] = INVALID_SOCK;
                        }
                    }
                }

            } // one client's socket
        } // for all sockets

        // Yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(YIELD_TO_ALL_MS));
    }

error:
    if (listen_sock != INVALID_SOCK) {
        close(listen_sock);
    }

    for (int i=0; i<max_socks; ++i) {
        if (sock[i] != INVALID_SOCK) {
            close(sock[i]);
        }
    }

    free(address_info);
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
static void hid_host_generic_report_callback(const uint8_t *const data, const int length)
{
    ESP_LOGI(TAG, "=== HID REPORT RECEIVED ===");
    ESP_LOGI(TAG, "Length: %d bytes", length);
    ESP_LOGI(TAG, "Raw data:");
    
    // Display raw bytes
    for (int i = 0; i < length && i < 32; i++) {  // Limit to 32 bytes for readability
        printf("%02X ", data[i]);
        if ((i + 1) % 8 == 0) printf("\n");
    }
    if (length > 32) {
        printf("... (truncated, total %d bytes)\n", length);
    } else {
        printf("\n");
    }
    
    // Show first few bytes as decimal too
    if (length > 0) {
        ESP_LOGI(TAG, "First 8 bytes as decimal:");
        for (int i = 0; i < length && i < 8; i++) {
            printf("%3d ", data[i]);
        }
        printf("\n");
    }
    
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
                ESP_LOGI(TAG, "UPS data detected, sending to parsing logic");
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

void refresh_ups_status_from_hid(bool *beep)
{
    /**
     * Generic UPS status parsing that supports multiple UPS models
     * 
     * This function now uses the generic parsing approach instead of hardcoded
     * SANTAK TG-BOX 850 specific protocol.
     * 
     */
    ups_data_t data;
    esp_err_t ret = parse_ups_data_generic(latest_hid_device_handle, &data);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse UPS data");
        
        return;
    }
    
    bool all_good_flag = data.good && !data.internal_failure && !data.need_replacement;

    char alert_text[32] = "";
    if (!all_good_flag)
    {
        strcat(alert_text, "[Suboptimal] ");
    }
    if (data.overload)
    {
        strcat(alert_text, "[Overload] ");
    }
    if (data.shutdown_imminent)
    {
        strcat(alert_text, "[Shutdown Imminent] ");
    }

    // Data is already parsed in the generic function above
    *beep = data.beep_enabled;

    // Update JSON with parsed data using generic function
    update_json_with_ups_data(&data);

    ESP_LOGI(TAG, "%sExternal Power: %s, Charging: %s, Discharging: %s, Buzzer: %s, Battery: %d%%, Load: %d%%, Remaining Runtime: %lu s(%.2f min)",
        alert_text,
        data.ac_present ? "ON" : "OFF",
        data.charging ? "Y" : "N",
        data.discharging ? "Y" : "N",
        data.beep_enabled ? "ON" : "OFF",
        data.battery_charge,
        data.ups_load,
        (unsigned long)data.battery_runtime,
        1.0 * data.battery_runtime / 60);
    
    if (all_good_flag && data.ac_present)
    {
        led_strip_set_pixel(led_strip, 0, 0, 0x03, 0);
        /* Refresh the strip to send data */
        led_strip_refresh(led_strip);
    }
    else
    {
        led_strip_set_pixel(led_strip, 0, 0x10, 0, 0);
        /* Refresh the strip to send data */
        led_strip_refresh(led_strip);
    }
    
    latest_ups_data = data;
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
    hid_host_event_queue = xQueueCreate(10, sizeof(hid_host_event_queue_t));

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

static uint8_t extract_field_value(const uint8_t *data, const hid_report_mapping_t *mapping)
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

static uint32_t extract_multi_byte_value(const uint8_t *data, const hid_report_mapping_t *mapping)
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

static esp_err_t parse_ups_data_generic(hid_host_device_handle_t device_handle, ups_data_t *data)
{
    if (!model_detected || detected_model_index >= MAX_UPS_MODELS) {
        ESP_LOGE(TAG, "No UPS model detected");
        return ESP_ERR_INVALID_STATE;
    }
    
    ups_model_config_t *model = &ups_models[detected_model_index];
    uint8_t recv[MAX_REPORT_SIZE];
    size_t len;
    
    memset(data, 0, sizeof(ups_data_t));
    
    ESP_LOGI(TAG, "[PARSER] Model: %s, Mapping count: %d", model->model_name, model->mapping_count);
    
    // Check if we have recent live data (within last 5 seconds)
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool use_live_data = live_data_available && (current_time - last_live_data_time) < 5000;
    
    if (use_live_data) {
        ESP_LOGI(TAG, "[PARSER] Using live HID data (age: %lu ms)", current_time - last_live_data_time);
    } else {
        ESP_LOGW(TAG, "[PARSER] No recent live data, using stored data");
    }
    
    // Parse each field using the model's mappings
    for (int i = 0; i < model->mapping_count; i++) {
        hid_report_mapping_t *mapping = &model->mappings[i];
        
        ESP_LOGI(TAG, "[PARSER] ReportID: 0x%02X, FieldType: %d, Offset: %d, Size: %d", 
                 mapping->report_id, mapping->field_type, mapping->data_offset, mapping->data_size);
        
        // Try to use live data first
        if (use_live_data && live_hid_lengths[mapping->report_id] > 0) {
            len = live_hid_lengths[mapping->report_id];
            memcpy(recv, live_hid_data[mapping->report_id], len);
            ESP_LOGI(TAG, "[PARSER] Using live data for report 0x%02X", mapping->report_id);
        } else {
            // Fallback to stored debug data
            if (debug_data_available && mapping->report_id < 16 && debug_report_lengths[mapping->report_id] > 0) {
                len = debug_report_lengths[mapping->report_id];
                memcpy(recv, debug_report_data[mapping->report_id], len);
                ESP_LOGI(TAG, "[PARSER] Using stored debug data for report 0x%02X", mapping->report_id);
            } else {
                ESP_LOGW(TAG, "[PARSER] No data available for report 0x%02X, skipping", mapping->report_id);
                continue;
            }
        }
        
        // Parse the field based on its type
        switch (mapping->field_type) {
            case 0: // Status
                {
                    uint8_t status = extract_field_value(recv, mapping);
                    ESP_LOGI(TAG, "[PARSER] Status byte: 0x%02X", status);
                    data->ac_present = (status & UPS_STATUS_AC_PRESENT) != 0;
                    data->charging = (status & UPS_STATUS_CHARGING) != 0;
                    data->discharging = (status & UPS_STATUS_DISCHARGING) != 0;
                    data->good = (status & UPS_STATUS_GOOD) != 0;
                    data->internal_failure = (status & UPS_STATUS_INTERNAL_FAILURE) != 0;
                    data->need_replacement = (status & UPS_STATUS_NEED_REPLACEMENT) != 0;
                    data->overload = (status & UPS_STATUS_OVERLOAD) != 0;
                    data->shutdown_imminent = (status & UPS_STATUS_SHUTDOWN_IMMINENT) != 0;
                }
                break;
            case 1: // Battery charge
                {
                    uint8_t raw_battery = extract_field_value(recv, mapping);
                    ESP_LOGI(TAG, "[PARSER] Raw battery: %d, Scaled: %d", raw_battery, (uint8_t)(raw_battery * model->battery_scale_factor));
                    data->battery_charge = (uint8_t)(raw_battery * model->battery_scale_factor);
                    if (data->battery_charge > 100) {
                        data->battery_charge = 100;
                    }
                }
                break;
            case 2: // Runtime
                {
                    uint32_t raw_runtime = extract_multi_byte_value(recv, mapping);
                    ESP_LOGI(TAG, "[PARSER] Raw runtime: %lu, Scaled: %lu", (unsigned long)raw_runtime, (unsigned long)(raw_runtime * model->runtime_scale_factor));
                    data->battery_runtime = (uint32_t)(raw_runtime * model->runtime_scale_factor);
                }
                break;
            case 3: // Load
                {
                    uint8_t raw_load = extract_field_value(recv, mapping);
                    ESP_LOGI(TAG, "[PARSER] Raw load: %d, Scaled: %d", raw_load, (uint8_t)(raw_load * model->load_scale_factor));
                    data->ups_load = (uint8_t)(raw_load * model->load_scale_factor);
                    if (data->ups_load > 100) {
                        data->ups_load = 100;
                    }
                }
                break;
            case 4: // Voltage
                {
                    uint32_t voltage = extract_multi_byte_value(recv, mapping);
                    ESP_LOGI(TAG, "[PARSER] Voltage: %lu", (unsigned long)voltage);
                    data->actual_voltage = voltage;  // 232V is correct for Australia
                }
                break;
            case 5: // Alarm control
                {
                    uint8_t alarm = extract_field_value(recv, mapping);
                    ESP_LOGI(TAG, "[PARSER] Alarm control: %d", alarm);
                    data->audible_alarm_control = alarm;
                    data->beep_enabled = (alarm == model->beep_enable_value);
                }
                break;
        }
    }
    
    return ESP_OK;
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

static void update_json_with_ups_data(const ups_data_t *data)
{
    ESP_LOGI(TAG, "[JSON] Updating NUT JSON: status=%s, battery_charge=%d, runtime=%lu, load=%d, voltage=%d, beep=%s", 
        data->ac_present ? "OL" : "OB", data->battery_charge, (unsigned long)data->battery_runtime, data->ups_load, data->actual_voltage, data->beep_enabled ? "enabled" : "disabled");
    char setted_text[32];
    
    // Update UPS status
    if (data->ac_present) {
        strcpy(setted_text, "OL");
    } else {
        strcpy(setted_text, "OB");
    }
    if (data->shutdown_imminent) {
        strcat(setted_text, " LB");
    }
    if (!data->good || data->internal_failure || data->need_replacement) {
        strcat(setted_text, " RB");
    }
    if (data->overload) {
        strcat(setted_text, " OVER");
    }
    
    cJSON *got_item = cJSON_GetObjectItemCaseSensitive(json_object, "ups");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "status");
    cJSON_SetValuestring(got_item, setted_text);
    
    // Update battery charge
    itoa(data->battery_charge, setted_text, 10);
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "battery");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "charge");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "_root");
    cJSON_SetValuestring(got_item, setted_text);
    
    // Update charger status
    strcpy(setted_text, "");
    if (data->charging) {
        strcpy(setted_text, "charging");
    } else if (data->discharging) {
        strcpy(setted_text, "discharging");
    }
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "battery");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "charger");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "status");
    cJSON_SetValuestring(got_item, setted_text);
    
    // Update runtime
    itoa(data->battery_runtime, setted_text, 10);
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "battery");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "runtime");
    cJSON_SetValuestring(got_item, setted_text);
    
    // Update load
    itoa(data->ups_load, setted_text, 10);
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "ups");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "load");
    cJSON_SetValuestring(got_item, setted_text);
    
    // Update voltage
    itoa(data->actual_voltage, setted_text, 10);
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "output");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "voltage");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "_root");
    cJSON_SetValuestring(got_item, setted_text);
    
    // Update beeper status
    strcpy(setted_text, data->beep_enabled ? "enabled" : "disabled");
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "ups");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "beeper");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "status");
    cJSON_SetValuestring(got_item, setted_text);
}



static void __attribute__((unused)) debug_unknown_ups_model(hid_host_device_handle_t device_handle)
{
    ESP_LOGI(TAG, "=== DEBUG: Unknown UPS Model Detection ===");
    
    // Clear previous debug data
    memset(debug_report_data, 0, sizeof(debug_report_data));
    memset(debug_report_lengths, 0, sizeof(debug_report_lengths));
    debug_data_available = false;
    
    // Try common report IDs to see what data is available
    uint8_t report_ids[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    uint8_t recv[MAX_REPORT_SIZE];
    size_t len;
    
    for (int i = 0; i < sizeof(report_ids); i++) {
        len = MAX_REPORT_SIZE;
        memset(recv, 0xFF, len);
        
        esp_err_t ret = hid_class_request_get_report(device_handle, 0x03, report_ids[i], recv, &len);
        if (ret == ESP_OK && len > 0) {
            ESP_LOGI(TAG, "Report 0x%02X (%d bytes):", report_ids[i], len);
            for (int j = 0; j < len && j < 16; j++) {
                printf("%02X ", recv[j]);
            }
            printf("\n");
            
            // Store the data for later use
            if (len <= MAX_REPORT_SIZE) {
                memcpy(debug_report_data[report_ids[i]], recv, len);
                debug_report_lengths[report_ids[i]] = len;
                debug_data_available = true;
            }
        } else if (ret != ESP_OK) {
            // Log the error but don't crash
            ESP_LOGW(TAG, "Failed to get report 0x%02X: %s", report_ids[i], esp_err_to_name(ret));
        }
    }
    
    ESP_LOGI(TAG, "=== End Debug ===");
}



