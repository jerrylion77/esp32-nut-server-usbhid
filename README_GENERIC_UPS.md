# Generic UPS HID Parsing System

This ESP32-S3 project now supports a generic HID parsing approach that can work with multiple UPS models, including the CyberPower VP700ELD and other UPS devices.

## Features

- **Multi-Model Support**: Automatically detects and configures different UPS models
- **Generic Fallback**: Works with unknown UPS models using a generic parsing approach
- **Easy Configuration**: Add new UPS models without modifying the main code
- **Debug Support**: Built-in debugging for unknown UPS models
- **Backward Compatibility**: Still supports the original SANTAK TG-BOX 850

## Supported UPS Models

### Currently Supported
1. **SANTAK TG-BOX 850** (Original model)
   - Vendor ID: 0x0463 (EATON)
   - Product ID: 0xFFFF

2. **CyberPower VP700ELD**
   - Vendor ID: 0x0764 (CyberPower)
   - Product ID: 0x0501

3. **CyberPower CP1500PFCLCD**
   - Vendor ID: 0x0764 (CyberPower)
   - Product ID: 0x0502

4. **Generic UPS** (Fallback for unknown models)
   - Automatically used for any unrecognized UPS

## How It Works

### 1. Model Detection
When a UPS is connected, the system:
1. Reads the USB device's Vendor ID and Product ID
2. Matches against known UPS models in the configuration
3. Falls back to generic parsing if no exact match is found

### 2. Generic Parsing
The system uses a flexible mapping approach:
- **Report Mappings**: Defines which HID report IDs contain specific data
- **Field Types**: Maps data fields to UPS parameters (battery, voltage, etc.)
- **Data Extraction**: Automatically extracts values based on byte offsets and sizes

### 3. Fallback Mechanism
For unknown UPS models:
- Uses a generic configuration with common report IDs
- Tries alternative report IDs if standard ones fail
- Provides debug output to help identify the correct format

## Adding New UPS Models

### Step 1: Identify Your UPS
1. Connect your UPS to the ESP32-S3
2. Check the debug output in the serial console
3. Note the Vendor ID and Product ID
4. Record the HID report data format

### Step 2: Add Configuration
Edit `main/ups_models_config.h` and add a new configuration:

```c
#define YOUR_UPS_MODEL_CONFIG { \
    .model_name = "Your UPS Model Name", \
    .vendor_id = 0xXXXX, /* Your UPS Vendor ID */ \
    .product_id = 0xXXXX, /* Your UPS Product ID */ \
    .status_report_id = 0x01, \
    .battery_report_id = 0x02, \
    .runtime_report_id = 0x03, \
    .load_report_id = 0x04, \
    .voltage_report_id = 0x05, \
    .alarm_report_id = 0x06, \
    .beep_report_id = 0x07, \
    .beep_enable_value = 0x01, \
    .beep_disable_value = 0x00, \
    .mappings = { \
        {0x01, 0x03, 0, 1, 0}, /* Status */ \
        {0x02, 0x03, 0, 1, 1}, /* Battery charge */ \
        {0x03, 0x03, 0, 2, 2}, /* Runtime */ \
        {0x04, 0x03, 0, 1, 3}, /* Load */ \
        {0x05, 0x03, 0, 2, 4}, /* Voltage */ \
        {0x06, 0x03, 0, 1, 5}, /* Alarm control */ \
    }, \
    .mapping_count = 6 \
}
```

### Step 3: Update Initialization
In `main/esp32-nut-server-usbhid.c`, update the `init_generic_ups_models()` function:

```c
static esp_err_t init_generic_ups_models(void)
{
    memset(ups_models, 0, sizeof(ups_models));
    
    // Initialize UPS models using predefined configurations
    ups_models[0] = (ups_model_config_t)SANTAK_TG_BOX_850_CONFIG;
    ups_models[1] = (ups_model_config_t)CYBERPOWER_VP700ELD_CONFIG;
    ups_models[2] = (ups_model_config_t)GENERIC_UPS_CONFIG;
    ups_models[3] = (ups_model_config_t)CYBERPOWER_CP1500PFCLCD_CONFIG;
    ups_models[4] = (ups_model_config_t)YOUR_UPS_MODEL_CONFIG; // Add your model
    
    ESP_LOGI(TAG, "Initialized %d UPS models for generic parsing", 5);
    return ESP_OK;
}
```

## Understanding HID Report Mappings

### Mapping Structure
```c
typedef struct {
    uint8_t report_id;      // HID report ID (e.g., 0x01, 0x02, etc.)
    uint8_t report_type;    // 0x03 for Feature reports
    uint8_t data_offset;    // Byte offset in the report
    uint8_t data_size;      // Number of bytes for this field
    uint8_t field_type;     // Field type (0=status, 1=battery, etc.)
} hid_report_mapping_t;
```

### Field Types
- `0`: Status (AC present, charging, etc.)
- `1`: Battery charge percentage
- `2`: Battery runtime (seconds)
- `3`: UPS load percentage
- `4`: Output voltage
- `5`: Alarm control (beep settings)

### Example: SANTAK TG-BOX 850
```c
{0x01, 0x03, 1, 1, 0}  // Status at byte 1, 1 byte long
{0x06, 0x03, 1, 1, 1}  // Battery at byte 1 of report 0x06, 1 byte long
{0x06, 0x03, 2, 4, 2}  // Runtime at byte 2 of report 0x06, 4 bytes long
```

## Debugging Unknown UPS Models

When an unknown UPS is connected, the system will:
1. Log the Vendor ID and Product ID
2. Try to read common HID report IDs
3. Display the raw data for each successful report
4. Use generic parsing as a fallback

### Debug Output Example
```
I (1234) ups: === DEBUG: Unknown UPS Model Detection ===
I (1234) ups: Report 0x01 (4 bytes): 01 02 03 04
I (1234) ups: Report 0x02 (2 bytes): 64 00
I (1234) ups: Report 0x03 (4 bytes): E8 03 00 00
I (1234) ups: === End Debug ===
```

Use this output to determine the correct report IDs and data format for your UPS.

## Troubleshooting

### Common Issues

1. **No Data Retrieved**
   - Check if the UPS supports HID communication
   - Verify USB connection and power
   - Try different report IDs

2. **Incorrect Data**
   - Verify byte offsets and sizes in mappings
   - Check endianness (little vs big endian)
   - Confirm field type assignments

3. **Generic Model Used**
   - Add your UPS configuration to `ups_models_config.h`
   - Use debug output to determine correct format
   - Update the initialization function

### Getting Help

1. Enable debug output for unknown models
2. Record the Vendor ID, Product ID, and report data
3. Share the debug output for assistance

## Building and Flashing

```bash
# Build the project
idf.py build

# Flash to ESP32-S3
idf.py flash monitor
```

## Configuration

The system uses the same configuration as before:
- TCP server on port 3493 (configurable in `main/Kconfig.projbuild`)
- WiFi connection (configure in `connect_to_wifi()` function)
- LED status indicators
- Beep control via GPIO pin 0

## License

This project maintains the same license as the original ESP32-S3 USB HID UPS server. 