# ESP32-NUT-Server-USBHID - CyberPower Edition

**Fork of [ludoux/esp32-nut-server-usbhid](https://github.com/ludoux/esp32-nut-server-usbhid)**

A specialized ESP32-S3 firmware designed to communicate with **CyberPower UPS models** (specifically VP700ELCD/VP1000ELCD) via USB-HID and act as a NUT (Network UPS Tools) server for seamless Home Assistant integration.

## 🎯 **Project Goals**

This fork focuses on creating a **reliable, efficient, and CyberPower-optimized** ESP32 firmware that:

1. **Reliably connects to WiFi** with automatic reconnection ✅
2. **Accurately reads USB HID data** from CyberPower UPS devices ✅
3. **Implements proper CyberPower HID protocol** based on official MIB specifications ✅
4. **Provides stable NUT server** for Home Assistant integration ✅
5. **Includes modular LED status system** for visual feedback (planned)
6. **Maintains clean, maintainable code** with proper error handling ✅

## 🔧 **Key Improvements Over Original**

- **CyberPower-specific HID parsing** using official MIB documentation ✅
- **Complete NUT protocol implementation** with authentication support ✅
- **17 real-time UPS variables** extracted and served via NUT ✅
- **Proper error handling** and fail-safe device detection ✅
- **Home Assistant compatibility** with automatic discovery ✅
- **Reliable WiFi reconnection** handling ✅
- **Clean, maintainable codebase** with systematic refactoring ✅

## 📊 **Supported UPS Variables**

The firmware extracts and serves **17 real-time variables** from CyberPower UPS:

### **Battery Information**
- `battery.charge` - Battery level percentage (0-100%)
- `battery.runtime` - Runtime remaining in minutes
- `battery.temperature` - Battery temperature in °C
- `battery.type` - Battery type (PbAc)

### **Power Information**
- `input.voltage` - Input voltage in V
- `output.voltage` - Output voltage in V
- `ups.load` - Load percentage (0-100%)
- `ups.power.nominal` - Nominal power rating (700W)

### **Status Information**
- `ups.status` - Online/Offline status (OL/OB)
- `ups.status.flags` - Status flags from UPS
- `ups.system.status` - System status code
- `ups.extended.status` - Extended status information

### **Device Information**
- `device.mfr` - Manufacturer (CyberPower)
- `device.model` - Model (VP700ELCD)
- `device.type` - Device type (ups)
- `ups.firmware` - Firmware version

### **Control Information**
- `ups.alarm.control` - Alarm control settings
- `ups.beep.control` - Beep control settings

## 🎨 **LED Status System (Planned)**

The firmware will include a modular LED status system that can be called to change colors based on system state:

- **Green**: UPS connected and healthy
- **Orange**: UPS disconnected
- **Red**: UPS error or AC power loss
- **Blue**: WiFi connecting
- **White**: NUT client activity
- **Purple**: System error/reboot

## 🏗️ **Architecture**

```
┌─────────────────────────────────────┐
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
- [ ] LED status system
- [x] Production optimization

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

## 📚 **Documentation References**

- [USB HID Power Devices Specification](https://www.usb.org/sites/default/files/pdcv10_0.pdf)
- [CyberPower MIB Documentation](https://www.cyberpowersystems.com/products/software/mib-files/)
- [NUT CyberPower Driver](https://github.com/networkupstools/nut/blob/master/drivers/cyberpower-mib.c)
- [Original Project Documentation](https://github.com/ludoux/esp32-nut-server-usbhid)

## 🙏 **Credits**

**Original Author**: [ludoux](https://github.com/ludoux) - Created the base ESP32 USB-HID NUT server implementation

**This Fork**: Specialized for CyberPower UPS integration with Home Assistant, featuring improved reliability, accuracy, and maintainability.

## 📄 **License**

GPL-3.0 (inherited from original project)

---

**Note**: This firmware is now production-ready for CyberPower VP700ELCD/VP1000ELCD UPS models with Home Assistant integration.

For detailed technical information, video demos, and original implementation details, please visit the [original repository](https://github.com/ludoux/esp32-nut-server-usbhid).
