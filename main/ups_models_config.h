/*
 * UPS Models Configuration Header
 * 
 * This file contains configurations for different UPS models to support
 * generic HID parsing. Add new UPS models here without modifying the main code.
 */

#ifndef UPS_MODELS_CONFIG_H
#define UPS_MODELS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// UPS status flags
typedef enum {
    UPS_STATUS_AC_PRESENT = 0x01,
    UPS_STATUS_CHARGING = 0x02,
    UPS_STATUS_DISCHARGING = 0x04,
    UPS_STATUS_GOOD = 0x08,
    UPS_STATUS_INTERNAL_FAILURE = 0x10,
    UPS_STATUS_NEED_REPLACEMENT = 0x20,
    UPS_STATUS_OVERLOAD = 0x40,
    UPS_STATUS_SHUTDOWN_IMMINENT = 0x80
} ups_status_flags_t;

// HID report mapping structure
typedef struct {
    uint8_t report_id;
    uint8_t report_type;  // 0x01 = Input, 0x02 = Output, 0x03 = Feature
    uint8_t data_offset;
    uint8_t data_size;
    uint8_t field_type;   // 0 = status, 1 = battery_charge, 2 = runtime, 3 = load, 4 = voltage, 5 = alarm_control
} hid_report_mapping_t;

// UPS model configuration
typedef struct {
    char model_name[64];
    uint16_t vendor_id;
    uint16_t product_id;
    hid_report_mapping_t mappings[16];  // Support up to 16 different report mappings
    uint8_t mapping_count;
    uint8_t status_report_id;
    uint8_t battery_report_id;
    uint8_t runtime_report_id;
    uint8_t load_report_id;
    uint8_t voltage_report_id;
    uint8_t alarm_report_id;
    uint8_t beep_report_id;
    uint8_t beep_enable_value;
    uint8_t beep_disable_value;
    float battery_scale_factor;    // Scaling factor for battery percentage
    float load_scale_factor;       // Scaling factor for load percentage
    float runtime_scale_factor;    // Scaling factor for runtime
} ups_model_config_t;

// UPS data structure
typedef struct {
    bool ac_present;
    bool charging;
    bool discharging;
    bool good;
    bool internal_failure;
    bool need_replacement;
    bool overload;
    bool shutdown_imminent;
    uint8_t battery_charge;
    uint32_t battery_runtime;
    uint8_t ups_load;
    uint16_t actual_voltage;
    uint8_t audible_alarm_control;
    bool beep_enabled;
    uint8_t system_status;
    uint32_t extended_status;
    uint8_t temperature;
    uint32_t additional_sensor;
    // Additional fields for complete parsing
    uint8_t battery_byte2;      // 0x20 byte 2
    uint8_t battery_byte3;      // 0x20 byte 3
    uint8_t status_byte2;       // 0x21 byte 2
    uint16_t output_voltage;    // 0x23 bytes 3-4
    uint16_t temp_range1;       // 0x86 bytes 1-2
    uint16_t temp_range2;       // 0x87 bytes 1-2
} ups_data_t;

// Predefined UPS model configurations
// Add new UPS models here

// SANTAK TG-BOX 850 (original model)
#define SANTAK_TG_BOX_850_CONFIG { \
    .model_name = "SANTAK TG-BOX 850", \
    .vendor_id = 0x0463,  /* EATON */ \
    .product_id = 0xFFFF, \
    .status_report_id = 0x01, \
    .battery_report_id = 0x06, \
    .runtime_report_id = 0x06, \
    .load_report_id = 0x07, \
    .voltage_report_id = 0x0E, \
    .alarm_report_id = 0x1F, \
    .beep_report_id = 0x1F, \
    .beep_enable_value = 0x02, \
    .beep_disable_value = 0x01, \
    .battery_scale_factor = 1.0,   /* No scaling needed for original model */ \
    .load_scale_factor = 1.0,      /* No scaling needed for original model */ \
    .runtime_scale_factor = 1.0,   /* No scaling needed for original model */ \
    .mappings = { \
        {0x01, 0x03, 1, 1, 0}, /* Status */ \
        {0x06, 0x03, 1, 1, 1}, /* Battery charge */ \
        {0x06, 0x03, 2, 4, 2}, /* Runtime */ \
        {0x07, 0x03, 6, 1, 3}, /* Load */ \
        {0x0E, 0x03, 1, 2, 4}, /* Voltage */ \
        {0x1F, 0x03, 1, 1, 5}, /* Alarm control */ \
    }, \
    .mapping_count = 6 \
}

// CyberPower VP700ELD
#define CYBERPOWER_VP700ELD_CONFIG { \
    .model_name = "CyberPower VP700ELD", \
    .vendor_id = 0x0764,  /* CyberPower */ \
    .product_id = 0x0501, /* Common CyberPower product ID */ \
    .status_report_id = 0x01, \
    .battery_report_id = 0x02, \
    .runtime_report_id = 0x03, \
    .load_report_id = 0x04, \
    .voltage_report_id = 0x05, \
    .alarm_report_id = 0x06, \
    .beep_report_id = 0x07, \
    .beep_enable_value = 0x01, \
    .beep_disable_value = 0x00, \
    .battery_scale_factor = 1.0,   /* Default scaling */ \
    .load_scale_factor = 1.0,      /* Default scaling */ \
    .runtime_scale_factor = 1.0,   /* Default scaling */ \
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

// CyberPower CP1500PFCLCD
#define CYBERPOWER_CP1500PFCLCD_CONFIG { \
    .model_name = "CyberPower CP1500PFCLCD", \
    .vendor_id = 0x0764,  /* CyberPower */ \
    .product_id = 0x0502, /* Another common CyberPower product ID */ \
    .status_report_id = 0x01, \
    .battery_report_id = 0x02, \
    .runtime_report_id = 0x03, \
    .load_report_id = 0x04, \
    .voltage_report_id = 0x05, \
    .alarm_report_id = 0x06, \
    .beep_report_id = 0x07, \
    .beep_enable_value = 0x01, \
    .beep_disable_value = 0x00, \
    .battery_scale_factor = 1.0,   /* Default scaling */ \
    .load_scale_factor = 1.0,      /* Default scaling */ \
    .runtime_scale_factor = 1.0,   /* Default scaling */ \
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

// CyberPower VP1000ELCD (corrected based on actual live HID data)
#define CYBERPOWER_VP1000ELCD_CONFIG { \
    .model_name = "CyberPower VP1000ELCD", \
    .vendor_id = 0x0764,  /* CyberPower */ \
    .product_id = 0x0503, /* VP1000ELCD specific */ \
    .status_report_id = 0x21, \
    .battery_report_id = 0x20, \
    .runtime_report_id = 0x25, \
    .load_report_id = 0x20, \
    .voltage_report_id = 0x23, \
    .alarm_report_id = 0x80, \
    .beep_report_id = 0x80, \
    .beep_enable_value = 0x02, \
    .beep_disable_value = 0x00, \
    .battery_scale_factor = 1.0,    /* HID reads 100, should be 100% */ \
    .load_scale_factor = 1.0,       /* HID reads 134, should be 134% (overload condition) */ \
    .runtime_scale_factor = 60.0,   /* HID reads 18, should be ~18 minutes (18 * 60 = 1080 seconds) */ \
    .mappings = { \
        {0x21, 0x03, 1, 1, 0}, /* Status - byte 1 */ \
        {0x20, 0x03, 1, 1, 1}, /* Battery charge - byte 1 */ \
        {0x25, 0x03, 1, 1, 2}, /* Runtime - byte 1 (using report 0x25 instead of 0x22) */ \
        {0x20, 0x03, 2, 1, 3}, /* Load - byte 2 */ \
        {0x23, 0x03, 1, 2, 4}, /* Voltage - bytes 1-2 (little endian) */ \
        {0x80, 0x03, 1, 1, 5}, /* Alarm control - byte 1 */ \
    }, \
    .mapping_count = 6 \
}

// CyberPower VP1000ELCD Alternative (based on common CyberPower patterns)
#define CYBERPOWER_VP1000ELCD_ALT_CONFIG { \
    .model_name = "CyberPower VP1000ELCD (Alt)", \
    .vendor_id = 0x0764,  /* CyberPower */ \
    .product_id = 0x0503, /* VP1000ELCD specific */ \
    .status_report_id = 0x01, \
    .battery_report_id = 0x03, \
    .runtime_report_id = 0x05, \
    .load_report_id = 0x07, \
    .voltage_report_id = 0x08, \
    .alarm_report_id = 0x06, \
    .beep_report_id = 0x09, \
    .beep_enable_value = 0x01, \
    .beep_disable_value = 0x00, \
    .battery_scale_factor = 2.0,    /* Alternative scaling for battery */ \
    .load_scale_factor = 0.25,      /* Alternative scaling for load */ \
    .runtime_scale_factor = 0.3,    /* Alternative scaling for runtime */ \
    .mappings = { \
        {0x01, 0x03, 1, 1, 0}, /* Status - byte 1 */ \
        {0x03, 0x03, 1, 1, 1}, /* Battery charge - byte 1 */ \
        {0x05, 0x03, 2, 2, 2}, /* Runtime - bytes 2-3 (little endian) */ \
        {0x07, 0x03, 1, 1, 3}, /* Load - byte 1 */ \
        {0x08, 0x03, 1, 1, 4}, /* Voltage - byte 1 */ \
        {0x06, 0x03, 2, 1, 5}, /* Alarm control - byte 2 */ \
    }, \
    .mapping_count = 6 \
}

// Generic fallback model for unknown UPS
#define GENERIC_UPS_CONFIG { \
    .model_name = "Generic UPS", \
    .vendor_id = 0x0000,  /* Will match any vendor */ \
    .product_id = 0x0000, /* Will match any product */ \
    .status_report_id = 0x01, \
    .battery_report_id = 0x02, \
    .runtime_report_id = 0x03, \
    .load_report_id = 0x04, \
    .voltage_report_id = 0x05, \
    .alarm_report_id = 0x06, \
    .beep_report_id = 0x07, \
    .beep_enable_value = 0x01, \
    .beep_disable_value = 0x00, \
    .battery_scale_factor = 1.0,   /* Default scaling */ \
    .load_scale_factor = 1.0,      /* Default scaling */ \
    .runtime_scale_factor = 1.0,   /* Default scaling */ \
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

// Add more UPS models here as needed
// Example for a new UPS model:
/*
#define NEW_UPS_MODEL_CONFIG { \
    .model_name = "New UPS Model", \
    .vendor_id = 0xXXXX, \
    .product_id = 0xXXXX, \
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
        {0x01, 0x03, 0, 1, 0}, \
        {0x02, 0x03, 0, 1, 1}, \
        {0x03, 0x03, 0, 2, 2}, \
        {0x04, 0x03, 0, 1, 3}, \
        {0x05, 0x03, 0, 2, 4}, \
        {0x06, 0x03, 0, 1, 5}, \
    }, \
    .mapping_count = 6 \
}
*/

#endif // UPS_MODELS_CONFIG_H 