# ESP32-NUT-Server-USBHID - CyberPower Edition

**Fork of [ludoux/esp32-nut-server-usbhid](https://github.com/ludoux/esp32-nut-server-usbhid)**

A specialized ESP32-S3 firmware designed to communicate with **CyberPower UPS models** (specifically VP700ELCD/VP1000ELCD) via USB-HID and act as a NUT (Network UPS Tools) server for seamless Home Assistant integration.

## 🎯 **Project Goals**

This fork focuses on creating a **reliable, efficient, and CyberPower-optimized** ESP32 firmware that:

1. **Reliably connects to WiFi** with automatic reconnection
2. **Accurately reads USB HID data** from CyberPower UPS devices
3. **Implements proper CyberPower HID protocol** based on official MIB specifications
4. **Provides stable NUT server** for Home Assistant integration
5. **Includes modular LED status system** for visual feedback
6. **Maintains clean, maintainable code** with proper error handling

## 🔧 **Key Improvements Over Original**

- **CyberPower-specific HID parsing** using official MIB documentation
- **Single data flow architecture** (no dual data systems)
- **Proper error handling** and fail-safe device detection
- **Modular debugging system** for development and production
- **Accurate data scaling** based on CyberPower specifications
- **Reliable WiFi reconnection** handling
- **Clean, maintainable codebase** with systematic refactoring

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
│ 5. Home Assistant Integration       │
├─────────────────────────────────────┤
│ 4. NUT Server (TCP/3493)            │
├─────────────────────────────────────┤
│ 3. CyberPower HID Parser            │
├─────────────────────────────────────┤
│ 2. USB HID Communication            │
├─────────────────────────────────────┤
│ 1. WiFi Connection                  │
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
- [ ] WiFi connection layer (reliable)
- [ ] USB HID reading layer (reliable)
- [ ] CyberPower-specific parsing
- [ ] NUT server implementation
- [ ] Home Assistant integration
- [ ] LED status system
- [ ] Production optimization

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

**Note**: This is a development project. Use at your own risk in production environments.

For detailed technical information, video demos, and original implementation details, please visit the [original repository](https://github.com/ludoux/esp32-nut-server-usbhid).
