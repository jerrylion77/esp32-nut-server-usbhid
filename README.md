# ESP32-NUT-Server-USBHID - CyberPower Edition

**Fork of [ludoux/esp32-nut-server-usbhid](https://github.com/ludoux/esp32-nut-server-usbhid)**

A specialized ESP32-S3 firmware designed to communicate with **CyberPower UPS models** (specifically VP700ELCD/VP1000ELCD) via USB-HID and act as a NUT (Network UPS Tools) server for seamless Home Assistant integration.

## 🎯 **Project Goals**

This fork focuses on creating a **reliable, efficient, and CyberPower-optimized** ESP32 firmware that:

1. **Reliably connects to WiFi** with automatic reconnection ✅
2. **Accurately reads USB HID data** from CyberPower UPS devices ✅
3. **Implements CyberPower HID protocol** based on reverse engineering ✅
4. **Provides stable NUT server** for Home Assistant integration ✅
5. **Includes modular LED status system** for visual feedback ✅
6. **Webserver: in progress (WiFi settings, factory restore, WiFi status, and UPS status are complete; TCP status, system health, and overall device status are next)**
7. **Maintains clean, maintainable code** with proper error handling ✅

## 🔧 **Key Improvements Over Original**

- **CyberPower-specific HID parsing** using reverse engineering and protocol analysis ✅
- **Complete NUT protocol implementation** with authentication support ✅
- **18 UPS variables** extracted and served via NUT (12 dynamic + 6 static) ✅
- **Proper error handling** and fail-safe device detection ✅
- **Home Assistant compatibility** with automatic discovery ✅
- **Reliable WiFi reconnection** handling ✅
- **Clean, maintainable codebase** with systematic refactoring ✅

## 📊 **Supported UPS Variables**

The firmware serves **18 variables** via NUT protocol:

### **12 Dynamic Variables** (parsed from UPS data)
- `battery.charge` - Battery level percentage (0-100%)
- `battery.runtime` - Runtime remaining in minutes
- `battery.temperature` - Battery temperature in °C
- `input.voltage` - Input voltage in V
- `output.voltage` - Output voltage in V
- `ups.load` - Load percentage (0-100%)
- `ups.status` - Online/Offline status (OL/UNKNOWN)
- `ups.status.flags` - Status flags from UPS
- `ups.system.status` - System status code
- `ups.extended.status` - Extended status information
- `ups.alarm.control` - Alarm control settings
- `ups.beep.control` - Beep control settings

### **6 Static Variables** (hardcoded for compatibility)
- `device.mfr` - Manufacturer (CyberPower)
- `device.model` - Model (VP700ELCD)
- `device.type` - Device type (ups)
- `ups.firmware` - Firmware version (1.0)
- `battery.type` - Battery type (PbAc)
- `ups.power.nominal` - Nominal power rating (700W)

### **6 Additional Parsed Fields** (for debugging/development)
- `battery_byte2`, `battery_byte3` - Raw battery data bytes
- `status_byte2` - Raw status data byte
- `temp_range1`, `temp_range2` - Temperature range data
- `additional_sensor` - Additional sensor data

**Note:** Home Assistant NUT integration currently recognizes only **10 entities** despite the firmware providing 18 variables. This is a limitation of the Home Assistant integration, not the firmware.

## ⚠️ **Known Limitations**

### **Protocol Reverse Engineering**
- The UPS HID protocol parsing is based on **reverse engineering** and may contain inaccuracies
- No official manufacturer documentation (MIB files) was used for this implementation
- Some data fields may be misinterpreted or missing
- **Contributions to improve protocol understanding are welcome!**

### **Home Assistant Integration**
- Home Assistant NUT integration only recognizes **10 entities** out of 18 available variables
- Recognized entities: Battery charge, Input voltage, Load, Output voltage, Status, Status data, Battery chemistry, Battery runtime, Battery temperature, Nominal power
- Additional variables (status flags, system status, alarm/beep controls) are available via NUT but not exposed in Home Assistant

### **Supported Models**
- Currently tested with CyberPower VP700ELCD and VP1000ELCD
- Other CyberPower models may work but are untested
- Non-CyberPower UPS models are not supported

## 🎨 **LED Status System ✅**

The firmware includes a comprehensive LED status system that provides real-time visual feedback:

- **Green**: All systems healthy (WiFi connected + UPS active)
- **Yellow**: Partial system issues (WiFi or UPS has problems)
- **Red**: Critical failure (both WiFi and UPS disconnected)
- **White**: Data activity pulse (indicates fresh UPS data received)

The LED automatically updates based on WiFi connection status and UPS data freshness, with a pulsing white indicator when new UPS data is received.

## 🏗️ **Architecture**

```
┌─────────────────────────────────────┐
│ 7. Webserver: In progress            │
├─────────────────────────────────────┤
│ 6. LED Status System ✅             │
├─────────────────────────────────────┤
│ 5. Home Assistant Integration ✅     │
├─────────────────────────────────────┤
│ 4. NUT Server (TCP/3493) ✅         │
├─────────────────────────────────────┤
│ 3. CyberPower HID Parser ✅         │
├─────────────────────────────────────┤
│ 2. USB HID Communication ✅         │
├─────────────────────────────────────┤
│ 1. WiFi Connection ✅               │
└─────────────────────────────────────┘
```

## 📋 **Requirements**

### Hardware
- ESP32-S3 development board with USB-OTG capability
- USB OTG cable for UPS connection
- CyberPower VP700ELCD or VP1000ELCD UPS

### Software
- ESP-IDF v5.1 or later
- Home Assistant with NUT integration

## 🚀 **Development Status**

- [x] Repository setup and backup
- [x] WiFi connection layer (reliable)
- [x] USB HID reading layer (reliable)
- [x] CyberPower-specific parsing
- [x] NUT server implementation
- [x] Home Assistant integration
- [x] LED status system
- [x] Production optimization
- [ ] Webserver for configuration and monitoring

## 🔧 **Setup Instructions**

### **1. Build and Flash**
```bash
idf.py build flash monitor
```

### **2. Configure WiFi**
The ESP32 will attempt to connect to WiFi. Monitor the output to see connection status.

**WiFi Credentials:**
1. Copy `main/wifi_secrets_example.h` to `main/wifi_secrets.h`:
   ```bash
   cp main/wifi_secrets_example.h main/wifi_secrets.h
   ```
2. Edit `main/wifi_secrets.h` and fill in your WiFi SSID and password:
   ```c
   #define WIFI_SSID      "YourNetwork"
   #define WIFI_PASSWORD  "YourPassword"
   ```
3. **Do not commit this file to git!** It is already listed in `.gitignore` for your safety.

### **3. Home Assistant Integration**
1. Add NUT integration in Home Assistant
2. Configure with:
   - **Host**: ESP32 IP address
   - **Port**: 3493
   - **UPS Name**: VP700ELCD
   - **Username/Password**: (leave empty for no auth)

### **4. Test Connection**
```bash
# Test with upsc
upsc VP700ELCD@<ESP32_IP>

# Test with netcat
nc <ESP32_IP> 3493
LIST UPS
LIST VAR VP700ELCD
```

## 🤝 **Contributing**

We welcome contributions to improve this project! Areas that need help:

### **Protocol Improvements**
- **UPS Protocol Analysis**: Help improve the reverse-engineered HID protocol parsing
- **Additional UPS Models**: Test and add support for other CyberPower models
- **Data Field Validation**: Verify the accuracy of parsed data fields

### **Feature Development**
- **Webserver Implementation**: Help build the planned configuration web interface
- **Enhanced Monitoring**: Add more detailed UPS diagnostics and alerts
- **Performance Optimization**: Improve parsing efficiency and memory usage

### **Documentation**
- **Setup Guides**: Create guides for different UPS models and configurations
- **Troubleshooting**: Document common issues and solutions
- **API Documentation**: Document the NUT server implementation

### **Testing**
- **Hardware Testing**: Test with different CyberPower UPS models
- **Integration Testing**: Verify Home Assistant and other NUT client compatibility
- **Stress Testing**: Test reliability under various network and power conditions

**How to Contribute:**
1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## 📚 **Documentation References**

- [USB HID Power Devices Specification](https://www.usb.org/sites/default/files/pdcv10_0.pdf)
- [NUT CyberPower Driver](https://github.com/networkupstools/nut/blob/master/drivers/cyberpower-mib.c)
- [Original Project Documentation](https://github.com/ludoux/esp32-nut-server-usbhid)

## 🙏 **Credits**

**Original Author**: [ludoux](https://github.com/ludoux) - Created the base ESP32 USB-HID NUT server implementation

**This Fork**: Specialized for CyberPower UPS integration with Home Assistant, featuring improved reliability, accuracy, and maintainability.

## 📄 **License**

GPL-3.0 (inherited from original project)

---

**Note**: This firmware is now production-ready for CyberPower VP700ELCD/VP1000ELCD UPS models with Home Assistant integration. The LED status system provides real-time visual feedback for system health monitoring.

For detailed technical information, video demos, and original implementation details, please visit the [original repository](https://github.com/ludoux/esp32-nut-server-usbhid).
