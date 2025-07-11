#include "webserver.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include <inttypes.h>
#include "esp_http_client.h"
#include "esp_timer.h"

// Add UPS state enum definition for use in this file
typedef enum {
    UPS_DISCONNECTED = 0,
    UPS_CONNECTED_WAITING_DATA,
    UPS_CONNECTED_ACTIVE,
    UPS_CONNECTED_STALE
} ups_connection_state_t;

static const char *TAG = "webserver";
static httpd_handle_t server = NULL;

// NVS namespace for WiFi credentials
#define WIFI_NVS_NAMESPACE "wifi_config"
#define WIFI_SSID_KEY "ssid"
#define WIFI_PASS_KEY "password"

// Helper for unique request IDs
static volatile uint32_t webserver_req_counter = 0;

// --- Webserver resilience logic state ---
#define ACCEPT_ERROR_THRESHOLD 10
#define ACCEPT_ERROR_WINDOW_MS 10000
#define FREE_HEAP_CRITICAL (16 * 1024)
#define SELF_CHECK_URL "http://127.0.0.1/api/wifi_status"
#define FREE_HEAP_LOG_INTERVAL_MS 20000

static int accept_error_counter = 0;
static int64_t accept_error_first_ts = 0;
static esp_timer_handle_t free_heap_log_timer = NULL;

// Forward declarations
static void log_free_heap(void* arg);
static void reset_accept_error_state(void);
void handle_accept_error(void);
static bool perform_self_check(void);

// HTML content for the main page
static const char* html_content = 
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
    "    <title>ESP32 UPS Monitor & Configuration</title>\n"
    "    <style>\n"
    "        * { margin: 0; padding: 0; box-sizing: border-box; }\n"
    "        body { font-family: 'Segoe UI', sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }\n"
    "        .container { max-width: 800px; margin: 0 auto; background: white; border-radius: 15px; box-shadow: 0 20px 40px rgba(0,0,0,0.1); overflow: hidden; }\n"
    "        .header { background: linear-gradient(135deg, #2c3e50 0%, #34495e 100%); color: white; padding: 30px; text-align: center; }\n"
    "        .header h1 { font-size: 2.5em; margin-bottom: 10px; }\n"
    "        .content { padding: 30px; }\n"
    "        .section { background: #f8f9fa; border-radius: 10px; padding: 25px; margin-bottom: 25px; border-left: 5px solid #3498db; }\n"
    "        .section h2 { color: #2c3e50; margin-bottom: 20px; font-size: 1.5em; }\n"
    "        .form-group { margin-bottom: 20px; }\n"
    "        .form-group label { display: block; margin-bottom: 8px; font-weight: 600; color: #2c3e50; }\n"
    "        .form-group input { width: 100%; padding: 12px; border: 2px solid #e9ecef; border-radius: 8px; font-size: 16px; }\n"
    "        .btn { padding: 12px 24px; border: none; border-radius: 8px; font-size: 16px; font-weight: 600; cursor: pointer; margin-right: 10px; }\n"
    "        .btn-primary { background: #3498db; color: white; }\n"
    "        .btn-secondary { background: #95a5a6; color: white; }\n"
    "        .btn-danger { background: #e74c3c; color: white; }\n"
    "        .status { padding: 15px; border-radius: 8px; margin-bottom: 20px; }\n"
    "        .status.success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }\n"
    "        .status.error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }\n"
    "        .status-indicator { display: inline-block; width: 14px; height: 14px; border-radius: 50%; margin-right: 8px; vertical-align: middle; background: #bbb; }\n"
    "        .status-indicator.green { background: #27ae60; }\n"
    "        .status-indicator.red { background: #e74c3c; }\n"
    "        .status-indicator.yellow { background: #f1c40f; }\n"
    "        .signal-bar { display: inline-block; width: 8px; height: 18px; margin-right: 2px; background: #dfe6e9; border-radius: 2px 2px 0 0; vertical-align: bottom; opacity: 0.5; transition: background 0.2s, opacity 0.2s; }\n"
    "        .signal-bar.active { background: #3498db; opacity: 1; }\n"
        "        .signal-inline { display: flex; align-items: center; font-size: 1.1em; color: #495057; gap: 8px; }\n"
        "        .signal-bars { display: flex; gap: 3px; justify-content: center; }\n"
        "        .signal-bar-new { width: 8px; height: 20px; border-radius: 2px; background: #e9ecef; transition: all 0.3s ease; }\n"
        "        .signal-bar-new.active { animation: pulse 2s infinite; }\n"
        "        .signal-bar-new.active.strong { background: #28a745; box-shadow: 0 0 10px rgba(40, 167, 69, 0.3); }\n"
        "        .signal-bar-new.active.medium { background: #ffc107; box-shadow: 0 0 10px rgba(255, 193, 7, 0.3); }\n"
        "        .signal-bar-new.active.weak { background: #dc3545; box-shadow: 0 0 10px rgba(220, 53, 69, 0.3); }\n"
        "        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.7; } }\n"
        "        .signal-percentage { font-size: 1em; font-weight: 500; color: #495057; margin-left: 6px; }\n"
    "        .footer { background: #ecf0f1; padding: 20px 30px; text-align: center; color: #7f8c8d; }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "    <div class=\"container\">\n"
    "        <div class=\"header\">\n"
    "            <h1>ESP32 UPS Monitor & Configuration</h1>\n"
    "            <p>WiFi Configuration</p>\n"
    "        </div>\n"
    "        <div class=\"content\">\n"
    "            <div class=\"section\">\n"
    "                <h2>WiFi Configuration</h2>\n"
    "                <div id=\"status\"></div>\n"
    "                <form id=\"wifiForm\">\n"
    "                    <div class=\"form-group\">\n"
    "                        <label for=\"ssid\">WiFi SSID:</label>\n"
    "                        <input type=\"text\" id=\"ssid\" name=\"ssid\" placeholder=\"Enter WiFi network name\" required>\n"
    "                    </div>\n"
    "                    <div class=\"form-group\">\n"
    "                        <label for=\"password\">WiFi Password:</label>\n"
    "                        <input type=\"password\" id=\"password\" name=\"password\" placeholder=\"Enter WiFi password\" required>\n"
    "                    </div>\n"
    "                    <button type=\"submit\" class=\"btn btn-primary\">Save & Reboot</button>\n"
    "                    <button type=\"button\" class=\"btn btn-secondary\" onclick=\"rebootDevice()\">Reboot Only</button>\n"
    "                </form>\n"
    "            </div>\n"
    "            <div class=\"section\">\n"
    "                <h2>Reset to Factory Settings</h2>\n"
    "                <p><strong>If you can't connect to WiFi and need to reset to factory settings:</strong></p>\n"
    "                <ol style=\"margin-left: 20px; line-height: 1.6;\">\n"
    "                    <li>While the ESP32 is running, hold down the <strong>BOOT button</strong></li>\n"
    "                    <li>Keep holding for 5 seconds - you'll see the LED turn purple during the countdown</li>\n"
    "                    <li>When the LED turns <strong>blue</strong>, you can release the button</li>\n"
    "                    <li>Device will clear WiFi credentials and reboot automatically</li>\n"
    "                    <li>Device will reboot and connect using factory WiFi settings</li>\n"
    "                </ol>\n"
    "                <p style=\"margin-top: 15px; padding: 10px; background: #fff3cd; border: 1px solid #ffeaa7; border-radius: 5px; color: #856404;\">\n"
    "                    <strong>Note:</strong> This will clear any saved WiFi credentials and restore the original hardcoded settings from the firmware. The LED color changes provide visual feedback: Purple = counting down, Blue = safe to release.\n"
    "                </p>\n"
    "            </div>\n"
    "            <div class=\"section\" id=\"dashboard-section\">\n"
    "                <h2>System Status Dashboard</h2>\n"
    "                <div class=\"status-row\">\n"
    "                    <span id=\"wifi-indicator\" class=\"status-indicator\"></span>\n"
    "                    <span class=\"status-label\">WiFi Status:</span>\n"
    "                    <span id=\"wifi-status\" class=\"status-value\"></span>\n"
    "                </div>\n"
    "                <div id=\"wifi-details\" style=\"margin-left:32px; color:#636e72; font-size:0.97em; margin-bottom:10px;\"></div>\n"
    "                <div id=\"wifi-signal-container\" style=\"margin-left:32px; color:#636e72; font-size:0.97em; margin-bottom:10px;\"></div>\n"
    "                <div class=\"status-row\">\n"
    "                    <span id=\"ups-indicator\" class=\"status-indicator\"></span>\n"
    "                    <span class=\"status-label\">UPS Status:</span>\n"
    "                    <span id=\"ups-status\" class=\"status-value\"></span>\n"
    "                </div>\n"
    "                <div id=\"ups-details\" style=\"margin-left:32px; color:#636e72; font-size:0.97em; margin-bottom:10px;\"></div>\n"
    "                <div class=\"status-row\">\n"
    "                    <span id=\"tcp-indicator\" class=\"status-indicator\"></span>\n"
    "                    <span class=\"status-label\">TCP Status:</span>\n"
    "                    <span id=\"tcp-status\" class=\"status-value\"></span>\n"
    "                </div>\n"
    "                <div id=\"tcp-details\" style=\"margin-left:32px; color:#636e72; font-size:0.97em; margin-bottom:10px;\"></div>\n"
    "                <div class=\"status-row\">\n"
    "                    <span id=\"esp-indicator\" class=\"status-indicator\"></span>\n"
    "                    <span class=\"status-label\">ESP Health:</span>\n"
    "                    <span id=\"esp-status\" class=\"status-value\"></span>\n"
    "                </div>\n"
    "                <div id=\"esp-details\" style=\"margin-left:32px; color:#636e72; font-size:0.97em; margin-bottom:10px;\"></div>\n"
    "            </div>\n"
    "        </div>\n"
    "        <div class=\"footer\">\n"
    "            <p>ESP32 UPS Server - System Status and Configuration Interface</p>\n"
    "        </div>\n"
    "    </div>\n"
    "    <script>\n"
    "    document.getElementById('wifiForm').addEventListener('submit', function(e) {\n"
    "        e.preventDefault();\n"
    "        const formData = new FormData(this);\n"
    "        const data = {\n"
    "            ssid: formData.get('ssid'),\n"
    "            password: formData.get('password')\n"
    "        };\n"
    "        fetch('/config', {\n"
    "            method: 'POST',\n"
    "            headers: {\n"
    "                'Content-Type': 'application/json',\n"
    "            },\n"
    "            body: JSON.stringify(data)\n"
    "        })\n"
    "        .then(response => response.json())\n"
    "        .then(data => {\n"
    "            showStatus(data.success ? 'Configuration saved successfully! Device will reboot...' : 'Error: ' + data.message, data.success);\n"
    "            if (data.success) {\n"
    "                setTimeout(() => {\n"
    "                    window.location.reload();\n"
    "                }, 3000);\n"
    "            }\n"
    "        })\n"
    "        .catch(error => {\n"
    "            showStatus('Error: ' + error.message, false);\n"
    "        });\n"
    "    });\n"
    "    function rebootDevice() {\n"
    "        if (confirm('Are you sure you want to reboot the device?')) {\n"
    "            fetch('/reboot', {\n"
    "                method: 'POST'\n"
    "            })\n"
    "            .then(response => response.json())\n"
    "            .then(data => {\n"
    "                showStatus('Device rebooting...', true);\n"
    "                setTimeout(() => {\n"
    "                    window.location.reload();\n"
    "                }, 5000);\n"
    "            })\n"
    "            .catch(error => {\n"
    "                showStatus('Error: ' + error.message, false);\n"
    "            });\n"
    "        }\n"
    "    }\n"
    "    function showStatus(message, isSuccess) {\n"
    "        const statusDiv = document.getElementById('status');\n"
    "        statusDiv.className = 'status ' + (isSuccess ? 'success' : 'error');\n"
    "        statusDiv.textContent = message;\n"
    "    }\n"
    "    const TOTAL_BARS = 10;\n"
    "    function initializeSignalBars() {\n"
    "        const signalBarsContainer = document.getElementById('wifi-signal-container');\n"
    "        signalBarsContainer.innerHTML = '<span class=\"signal-inline\">Signal: <span class=\"signal-bars\" id=\"signalBars\"></span><span class=\"signal-percentage\" id=\"signalValue\">0%</span></span>';\n"
    "        const barsContainer = document.getElementById('signalBars');\n"
    "        barsContainer.innerHTML = '';\n"
    "        for (let i = 0; i < TOTAL_BARS; i++) {\n"
    "            const bar = document.createElement('div');\n"
    "            bar.className = 'signal-bar-new';\n"
    "            bar.id = `bar-${i}`;\n"
    "            barsContainer.appendChild(bar);\n"
    "        }\n"
    "    }\n"
    "    function setSignalValue(signalStrength) {\n"
    "        const signalValueElement = document.getElementById('signalValue');\n"
    "        if (signalValueElement) {\n"
    "            signalValueElement.textContent = signalStrength + '%';\n"
    "            const activeBars = Math.round((signalStrength / 100) * TOTAL_BARS);\n"
    "            for (let i = 0; i < TOTAL_BARS; i++) {\n"
    "                const bar = document.getElementById(`bar-${i}`);\n"
    "                if (bar) {\n"
    "                    bar.className = 'signal-bar-new';\n"
    "                    if (i < activeBars) {\n"
    "                        bar.classList.add('active');\n"
    "                        if (signalStrength >= 70) {\n"
    "                            bar.classList.add('strong');\n"
    "                        } else if (signalStrength >= 30) {\n"
    "                            bar.classList.add('medium');\n"
    "                        } else {\n"
    "                            bar.classList.add('weak');\n"
    "                        }\n"
    "                    }\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    function updateWifiStatus() {\n"
    "        fetch('/api/wifi_status').then(r => r.json()).then(data => {\n"
    "            var ind = document.getElementById(\"wifi-indicator\");\n"
    "            ind.className = 'status-indicator ' + (data.connected ? 'green' : 'red');\n"
    "            document.getElementById(\"wifi-status\").textContent = data.connected ? 'CONNECTED' : 'DISCONNECTED';\n"
    "            var percent = (typeof data.signal === 'number') ? Math.max(0, Math.min(100, 2 * (data.signal + 100))) : 0;\n"
    "            var details = data.connected\n"
    "                ? (data.ip + ' | ' + data.ssid + ' | ' + percent + '%')\n"
    "                : 'Not connected';\n"
    "            document.getElementById(\"wifi-details\").innerHTML = details;\n"
    "            if (data.connected) {\n"
    "                setSignalValue(percent);\n"
    "            } else {\n"
    "                setSignalValue(0);\n"
    "            }\n"
    "        });\n"
    "    }\n"
    "    function formatUptime(seconds) {\n"
    "        if (seconds < 60) {\n"
    "            return Math.floor(seconds) + ' seconds';\n"
    "        } else if (seconds < 3600) {\n"
    "            var minutes = Math.floor(seconds / 60);\n"
    "            var remainingSeconds = Math.floor(seconds % 60);\n"
    "            return minutes + ' minutes ' + remainingSeconds + ' seconds';\n"
    "        } else if (seconds < 86400) {\n"
    "            var hours = Math.floor(seconds / 3600);\n"
    "            var minutes = Math.floor((seconds % 3600) / 60);\n"
    "            var remainingSeconds = Math.floor(seconds % 60);\n"
    "            return hours + ' hours ' + minutes + ' minutes ' + remainingSeconds + ' seconds';\n"
    "        } else {\n"
    "            var days = Math.floor(seconds / 86400);\n"
    "            var hours = Math.floor((seconds % 86400) / 3600);\n"
    "            var minutes = Math.floor((seconds % 3600) / 60);\n"
    "            var remainingSeconds = Math.floor(seconds % 60);\n"
    "            return days + ' days ' + hours + ' hours ' + minutes + ' minutes ' + remainingSeconds + ' seconds';\n"
    "        }\n"
    "    }\n"
    "    function formatTimeAgo(seconds) {\n"
    "        if (seconds < 60) {\n"
    "            return Math.floor(seconds) + ' seconds ago';\n"
    "        } else if (seconds < 3600) {\n"
    "            var minutes = Math.floor(seconds / 60);\n"
    "            var remainingSeconds = Math.floor(seconds % 60);\n"
    "            return minutes + ' minutes ' + remainingSeconds + ' seconds ago';\n"
    "        } else if (seconds < 86400) {\n"
    "            var hours = Math.floor(seconds / 3600);\n"
    "            var minutes = Math.floor((seconds % 3600) / 60);\n"
    "            var remainingSeconds = Math.floor(seconds % 60);\n"
    "            return hours + ' hours ' + minutes + ' minutes ' + remainingSeconds + ' seconds ago';\n"
    "        } else {\n"
    "            var days = Math.floor(seconds / 86400);\n"
    "            var hours = Math.floor((seconds % 86400) / 3600);\n"
    "            var minutes = Math.floor((seconds % 3600) / 60);\n"
    "            var remainingSeconds = Math.floor(seconds % 60);\n"
    "            return days + ' days ' + hours + ' hours ' + minutes + ' minutes ' + remainingSeconds + ' seconds ago';\n"
    "        }\n"
    "    }\n"
    "    function updateUpsStatus() {\n"
    "        fetch('/api/ups_status').then(r => r.json()).then(data => {\n"
    "            var ind = document.getElementById(\"ups-indicator\");\n"
    "            ind.className = 'status-indicator ' + data.color;\n"
    "            var statusText = data.state;\n"
    "            var detailsText = '';\n"
    "            if (data.state === 'STALE' && typeof data.stale_duration_ms === 'number') {\n"
    "                var staleSeconds = Math.floor(data.stale_duration_ms / 1000);\n"
    "                statusText += ' (for ' + formatUptime(staleSeconds) + ')';\n"
    "            }\n"
    "            document.getElementById(\"ups-status\").textContent = statusText;\n"
    "            var seconds = data.ms_since_last / 1000;\n"
    "            var timeAgo = formatTimeAgo(seconds);\n"
    "            detailsText = 'Last message: ' + timeAgo;\n"
    "            document.getElementById(\"ups-details\").textContent = detailsText;\n"
    "        });\n"
    "    }\n"
    "    function updateTcpStatus() {\n"
    "        fetch('/api/tcp_status').then(r => r.json()).then(data => {\n"
    "            var ind = document.getElementById(\"tcp-indicator\");\n"
    "            var statusText = '';\n"
    "            var color = 'red';\n"
    "            var connectionsText = '';\n"
    "            if (data.running) {\n"
    "                if (data.connections > 0) {\n"
    "                    color = 'green';\n"
    "                    statusText = 'RUNNING';\n"
    "                    connectionsText = 'Connections: ' + data.connections + ' connection' + (data.connections !== 1 ? 's' : '');\n"
    "                } else {\n"
    "                    color = 'yellow';\n"
    "                    statusText = 'RUNNING';\n"
    "                    connectionsText = 'Connections: 0 connections';\n"
    "                }\n"
    "            } else {\n"
    "                color = 'red';\n"
    "                statusText = 'STOPPED';\n"
    "                connectionsText = 'Connections: 0 connections';\n"
    "            }\n"
    "            ind.className = 'status-indicator ' + color;\n"
    "            document.getElementById(\"tcp-status\").textContent = statusText;\n"
    "            document.getElementById(\"tcp-details\").textContent = connectionsText;\n"
    "        });\n"
    "    }\n"
    "    function updateEspHealthStatus() {\n"
    "        fetch('/api/esp_health').then(r => r.json()).then(data => {\n"
    "            var ind = document.getElementById(\"esp-indicator\");\n"
    "            var statusText = '';\n"
    "            var color = 'red';\n"
    "            if (data.memory_percent >= 65) {\n"
    "                color = 'green';\n"
    "                statusText = 'HEALTHY';\n"
    "            } else if (data.memory_percent >= 30) {\n"
    "                color = 'yellow';\n"
    "                statusText = 'WARNING';\n"
    "            } else {\n"
    "                color = 'red';\n"
    "                statusText = 'CRITICAL';\n"
    "            }\n"
    "            ind.className = 'status-indicator ' + color;\n"
    "            document.getElementById(\"esp-status\").textContent = statusText;\n"
    "            var freeKB = Math.floor(data.free_heap / 1024);\n"
    "            var totalKB = Math.floor(data.total_heap / 1024);\n"
    "            var memoryText = 'Free Memory: ' + freeKB + 'KB out of ' + totalKB + 'KB (' + data.memory_percent + '% Free)';\n"
    "            var uptimeText = 'Uptime: ' + formatUptime(data.uptime_seconds);\n"
    "            document.getElementById(\"esp-details\").innerHTML = memoryText + '<br>' + uptimeText;\n"
    "        });\n"
    "    }\n"
    "    initializeSignalBars();\n"
    "    updateWifiStatus();\n"
    "    updateUpsStatus();\n"
    "    updateTcpStatus();\n"
    "    updateEspHealthStatus();\n"
    "    setInterval(updateWifiStatus, 5000);\n"
    "    setInterval(updateUpsStatus, 5000);\n"
    "    setInterval(updateTcpStatus, 5000);\n"
    "    setInterval(updateEspHealthStatus, 5000);\n"
    "    </script>\n"
    "</body>\n"
    "</html>";

// --- Periodic free heap logging ---
static void log_free_heap(void* arg) {
    ESP_LOGI(TAG, "Free heap: %u bytes, Min free heap: %u bytes", (unsigned int)esp_get_free_heap_size(), (unsigned int)esp_get_minimum_free_heap_size());
}

static void start_free_heap_logging(void) {
    if (!free_heap_log_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = &log_free_heap,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "heaplog"
        };
        esp_timer_create(&timer_args, &free_heap_log_timer);
        esp_timer_start_periodic(free_heap_log_timer, FREE_HEAP_LOG_INTERVAL_MS * 1000);
    }
}

static void reset_accept_error_state(void) {
    accept_error_counter = 0;
    accept_error_first_ts = 0;
}

// --- Accept error burst detection ---
void handle_accept_error(void) {
    int64_t now = esp_timer_get_time() / 1000; // ms
    if (accept_error_counter == 0) {
        accept_error_first_ts = now;
        accept_error_counter = 1;
        return;
    }
    accept_error_counter++;
    if ((now - accept_error_first_ts) < ACCEPT_ERROR_WINDOW_MS) {
        if (accept_error_counter > ACCEPT_ERROR_THRESHOLD) {
            // Burst detected, proceed to memory check and self-check
            if (esp_get_free_heap_size() < FREE_HEAP_CRITICAL) {
                ESP_LOGE(TAG, "[RESILIENCE] Heap critically low (%u bytes), rebooting system", (unsigned int)esp_get_free_heap_size());
                esp_restart();
                return;
            }
            ESP_LOGW(TAG, "[RESILIENCE] Accept error burst detected, performing self-check");
            if (!perform_self_check()) {
                ESP_LOGE(TAG, "[RESILIENCE] Self-check failed, restarting webserver");
                if (server) {
                    httpd_stop(server);
                    server = NULL;
                }
                webserver_start();
            } else {
                ESP_LOGI(TAG, "[RESILIENCE] Self-check succeeded, not restarting webserver");
            }
            reset_accept_error_state();
        }
    } else {
        // Window expired, reset state
        reset_accept_error_state();
    }
}

// --- Self-check: always use new TCP connection ---
static bool perform_self_check(void) {
    esp_http_client_config_t config = {
        .url = SELF_CHECK_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 2000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Connection", "close");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && status == 200) {
        return true;
    }
    ESP_LOGW(TAG, "[RESILIENCE] Self-check HTTP error: %s, status: %d", esp_err_to_name(err), status);
    return false;
}

// --- TCP Status API Handler ---
extern int get_active_tcp_connections(void);
extern bool is_tcp_server_running(void);

static esp_err_t tcp_status_get_handler(httpd_req_t *req)
{
    uint32_t req_id = __atomic_add_fetch(&webserver_req_counter, 1, __ATOMIC_SEQ_CST);
    ESP_LOGI(TAG, "[REQ %lu] tcp_status_get_handler START uri=%s", (unsigned long)req_id, req->uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    char response[128];
    bool running = is_tcp_server_running();
    int connections = get_active_tcp_connections();
    snprintf(response, sizeof(response),
        "{\"running\":%s,\"connections\":%d}",
        running ? "true" : "false", connections);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "[REQ %lu] tcp_status_get_handler END", (unsigned long)req_id);
    return ESP_OK;
}

// --- ESP Health API Handler ---
static esp_err_t esp_health_get_handler(httpd_req_t *req)
{
    uint32_t req_id = __atomic_add_fetch(&webserver_req_counter, 1, __ATOMIC_SEQ_CST);
    ESP_LOGI(TAG, "[REQ %lu] esp_health_get_handler START uri=%s", (unsigned long)req_id, req->uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    
    // Get memory information
    size_t free_heap = esp_get_free_heap_size();
    
    // ESP32-S3 has approximately 512KB of SRAM, but actual available heap is less
    // Using a conservative estimate of 320KB total heap for calculation
    const size_t total_heap_estimate = 320 * 1024; // 320KB in bytes
    int memory_percent = (int)((free_heap * 100) / total_heap_estimate);
    
    // Get uptime in seconds
    int64_t uptime_us = esp_timer_get_time();
    int64_t uptime_seconds = uptime_us / 1000000;
    
    char response[256];
    snprintf(response, sizeof(response),
        "{\"free_heap\":%u,\"total_heap\":%u,\"memory_percent\":%d,\"uptime_seconds\":%lld}",
        (unsigned int)free_heap, (unsigned int)total_heap_estimate, memory_percent, (long long)uptime_seconds);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "[REQ %lu] esp_health_get_handler END", (unsigned long)req_id);
    return ESP_OK;
}

// WiFi configuration handler
static esp_err_t config_post_handler(httpd_req_t *req)
{
    uint32_t req_id = __atomic_add_fetch(&webserver_req_counter, 1, __ATOMIC_SEQ_CST);
    ESP_LOGI(TAG, "[REQ %lu] config_post_handler START uri=%s", (unsigned long)req_id, req->uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    char content[512];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        ESP_LOGW(TAG, "[REQ %lu] config_post_handler FAILED to receive data", (unsigned long)req_id);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        ESP_LOGI(TAG, "[REQ %lu] config_post_handler END (error)", (unsigned long)req_id);
        return ESP_FAIL;
    }
    content[received] = '\0';
    
    // Parse JSON (simple parsing for ssid and password)
    char ssid[64] = {0};
    char password[64] = {0};
    
    // Simple JSON parsing - look for "ssid" and "password" fields
    char *ssid_start = strstr(content, "\"ssid\":\"");
    char *pass_start = strstr(content, "\"password\":\"");
    
    if (ssid_start) {
        ssid_start += 8; // Skip "ssid":"
        char *ssid_end = strchr(ssid_start, '"');
        if (ssid_end) {
            int len = ssid_end - ssid_start;
            if (len < sizeof(ssid)) {
                strncpy(ssid, ssid_start, len);
                ssid[len] = '\0';
            }
        }
    }
    
    if (pass_start) {
        pass_start += 12; // Skip "password":"
        char *pass_end = strchr(pass_start, '"');
        if (pass_end) {
            int len = pass_end - pass_start;
            if (len < sizeof(password)) {
                strncpy(password, pass_start, len);
                password[len] = '\0';
            }
        }
    }
    
    if (strlen(ssid) == 0 || strlen(password) == 0) {
        ESP_LOGW(TAG, "[REQ %lu] config_post_handler MISSING SSID or password", (unsigned long)req_id);
        const char* error_response = "{\"success\":false,\"message\":\"Missing SSID or password\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_response, HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, "[REQ %lu] config_post_handler END (missing data)", (unsigned long)req_id);
        return ESP_OK;
    }
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[REQ %lu] config_post_handler Error opening NVS handle: %s", (unsigned long)req_id, esp_err_to_name(err));
        const char* error_response = "{\"success\":false,\"message\":\"Failed to open NVS\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_response, HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, "[REQ %lu] config_post_handler END (NVS error)", (unsigned long)req_id);
        return ESP_OK;
    }
    
    err = nvs_set_str(nvs_handle, WIFI_SSID_KEY, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[REQ %lu] config_post_handler Error saving SSID: %s", (unsigned long)req_id, esp_err_to_name(err));
        nvs_close(nvs_handle);
        const char* error_response = "{\"success\":false,\"message\":\"Failed to save SSID\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_response, HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, "[REQ %lu] config_post_handler END (NVS error)", (unsigned long)req_id);
        return ESP_OK;
    }
    
    err = nvs_set_str(nvs_handle, WIFI_PASS_KEY, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[REQ %lu] config_post_handler Error saving password: %s", (unsigned long)req_id, esp_err_to_name(err));
        nvs_close(nvs_handle);
        const char* error_response = "{\"success\":false,\"message\":\"Failed to save password\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_response, HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, "[REQ %lu] config_post_handler END (NVS error)", (unsigned long)req_id);
        return ESP_OK;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[REQ %lu] config_post_handler Error committing NVS: %s", (unsigned long)req_id, esp_err_to_name(err));
        const char* error_response = "{\"success\":false,\"message\":\"Failed to commit configuration\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_response, HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, "[REQ %lu] config_post_handler END (NVS error)", (unsigned long)req_id);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "[REQ %lu] config_post_handler END (success)", (unsigned long)req_id);
    const char* success_response = "{\"success\":true,\"message\":\"Configuration saved successfully\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, success_response, HTTPD_RESP_USE_STRLEN);
    
    // Schedule reboot after a short delay
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

// Reboot handler
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    uint32_t req_id = __atomic_add_fetch(&webserver_req_counter, 1, __ATOMIC_SEQ_CST);
    ESP_LOGI(TAG, "[REQ %lu] reboot_post_handler START uri=%s", (unsigned long)req_id, req->uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    const char* response = "{\"success\":true,\"message\":\"Rebooting...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    
    // Schedule reboot after a short delay
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    ESP_LOGI(TAG, "[REQ %lu] reboot_post_handler END", (unsigned long)req_id);
    return ESP_OK;
}

// Root page handler
static esp_err_t root_get_handler(httpd_req_t *req)
{
    uint32_t req_id = __atomic_add_fetch(&webserver_req_counter, 1, __ATOMIC_SEQ_CST);
    ESP_LOGI(TAG, "[REQ %lu] root_get_handler START uri=%s", (unsigned long)req_id, req->uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_content, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "[REQ %lu] root_get_handler END", (unsigned long)req_id);
    return ESP_OK;
}

// WiFi status handler
static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    uint32_t req_id = __atomic_add_fetch(&webserver_req_counter, 1, __ATOMIC_SEQ_CST);
    ESP_LOGI(TAG, "[REQ %lu] wifi_status_get_handler START uri=%s", (unsigned long)req_id, req->uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    wifi_ap_record_t ap_info;
    esp_netif_ip_info_t ip_info;
    char response[512];
    
    // Get WiFi connection status
    bool connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    
    if (connected) {
        // Get IP address
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(response, sizeof(response),
                "{\"connected\":true,\"ssid\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"signal\":%d}",
                ap_info.ssid,
                ip4_addr1_16(&ip_info.ip),
                ip4_addr2_16(&ip_info.ip),
                ip4_addr3_16(&ip_info.ip),
                ip4_addr4_16(&ip_info.ip),
                ap_info.rssi);
        } else {
            snprintf(response, sizeof(response),
                "{\"connected\":true,\"ssid\":\"%s\",\"ip\":\"unknown\",\"signal\":%d}",
                ap_info.ssid, ap_info.rssi);
        }
    } else {
        snprintf(response, sizeof(response),
            "{\"connected\":false,\"ssid\":\"\",\"ip\":\"\",\"signal\":0}");
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "[REQ %lu] wifi_status_get_handler END", (unsigned long)req_id);
    return ESP_OK;
}

// UPS status handler
static esp_err_t ups_status_get_handler(httpd_req_t *req)
{
    uint32_t req_id = __atomic_add_fetch(&webserver_req_counter, 1, __ATOMIC_SEQ_CST);
    ESP_LOGI(TAG, "[REQ %lu] ups_status_get_handler START uri=%s", (unsigned long)req_id, req->uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    extern ups_connection_state_t get_ups_state(void);
    extern unsigned int get_ups_last_data_time(void);
    extern uint32_t get_ups_stale_duration_ms(void);
    char response[160];
    const char *state_str = "UNKNOWN";
    const char *color = "red";
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t ms_since_last = now - get_ups_last_data_time();
    uint32_t stale_duration = 0;
    switch (get_ups_state()) {
        case 0: state_str = "DISCONNECTED"; color = "red"; break;
        case 1: state_str = "WAITING"; color = "yellow"; break;
        case 2: state_str = "ACTIVE"; color = "green"; break;
        case 3: state_str = "STALE"; color = "red"; stale_duration = get_ups_stale_duration_ms(); break;
    }
    if (stale_duration > 0) {
        snprintf(response, sizeof(response),
            "{\"state\":\"%s\",\"color\":\"%s\",\"ms_since_last\":%lu,\"stale_duration_ms\":%lu}",
            state_str, color, (unsigned long)ms_since_last, (unsigned long)stale_duration);
    } else {
        snprintf(response, sizeof(response),
            "{\"state\":\"%s\",\"color\":\"%s\",\"ms_since_last\":%lu}",
            state_str, color, (unsigned long)ms_since_last);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "[REQ %lu] ups_status_get_handler END", (unsigned long)req_id);
    return ESP_OK;
}

// Start the webserver
esp_err_t webserver_start(void)
{
    if (server) {
        ESP_LOGI(TAG, "Webserver already running");
        return ESP_OK;
    }
    start_free_heap_logging();
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);
        
        httpd_uri_t config_post = {
            .uri = "/config",
            .method = HTTP_POST,
            .handler = config_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &config_post);
        
        httpd_uri_t reboot_post = {
            .uri = "/reboot",
            .method = HTTP_POST,
            .handler = reboot_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &reboot_post);
        
        httpd_uri_t wifi_status = {
            .uri = "/api/wifi_status",
            .method = HTTP_GET,
            .handler = wifi_status_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_status);
        
        httpd_uri_t ups_status = {
            .uri = "/api/ups_status",
            .method = HTTP_GET,
            .handler = ups_status_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &ups_status);

        httpd_uri_t tcp_status = {
            .uri = "/api/tcp_status",
            .method = HTTP_GET,
            .handler = tcp_status_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &tcp_status);

        httpd_uri_t esp_health = {
            .uri = "/api/esp_health",
            .method = HTTP_GET,
            .handler = esp_health_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &esp_health);
        
        ESP_LOGI(TAG, "Webserver started on port %d", config.server_port);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to start webserver");
        return ESP_FAIL;
    }
} 

void webserver_restart(void) {
    ESP_LOGW(TAG, "Restarting webserver due to health check failure");
    // Stop the webserver if running
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    // Stop and delete the heap log timer if running
    if (free_heap_log_timer) {
        esp_timer_stop(free_heap_log_timer);
        esp_timer_delete(free_heap_log_timer);
        free_heap_log_timer = NULL;
    }
    // Start the webserver again
    webserver_start();
} 