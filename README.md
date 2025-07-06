# ESP32-NUT-Server-USBHID - CyberPower Edition

**Fork of [ludoux/esp32-nut-server-usbhid](https://github.com/ludoux/esp32-nut-server-usbhid)**

A specialized ESP32-S3 firmware designed to communicate with **CyberPower UPS models** (specifically VP700ELCD/VP1000ELCD) via USB-HID and act as a NUT (Network UPS Tools) server for seamless Home Assistant integration.

---

## 🚦 Project Status (May 2024)

- **Stable HID parsing for CyberPower VP1000ELCD** (and likely VP700ELCD)
- **Queue size increased** for better burst handling (20 events)
- **Verbose logging control** via compile-time flag
- **Clean build, no warnings**
- **Ready for NUT server integration**
- **VP700ELCD**: Similar report structure, but data scaling/interpretation not fully mapped

---

## ⚠️ Current Limitations

- **Data accuracy for CyberPower UPS models is not fully solved**
    - Some fields (load, runtime, status flags) are raw values, not yet reverse engineered
    - Scaling and field mapping for some values are not fully understood
- **No official CyberPower HID documentation available**
- **NUT server integration is in progress**

---

## 🤝 How to Contribute

- **Fork this project** and experiment with your own CyberPower UPS
- **Submit pull requests** for bug fixes, improvements, or new features
- **Open issues** for bugs, questions, or feature requests
- **Help with CyberPower HID data mapping!**
    - If you have access to documentation, reverse engineering skills, or other models, your help is especially welcome
- **See something odd in the data?** Please share your findings!

---

## 🗂️ Branching Strategy

- **master**: Always stable, last tested working code (currently VP1000 HID parser)
- **feature/ups-data-parsing**: Stable HID parsing and queue improvements
- **feature/nut-server-integration**: In development, NUT protocol and Home Assistant integration
- **Other feature branches**: For experimental or future work

---

## 🚧 Next Steps

- **NUT server implementation** (feature/nut-server-integration branch)
- **Home Assistant integration**
- **Data accuracy improvements** (help wanted!)
- **LED status system** (planned)

---

## 📚 Documentation References

- [USB HID Power Devices Specification](https://www.usb.org/sites/default/files/pdcv10_0.pdf)
- [CyberPower MIB Documentation](https://www.cyberpowersystems.com/products/software/mib-files/)
- [NUT CyberPower Driver](https://github.com/networkupstools/nut/blob/master/drivers/cyberpower-mib.c)
- [Original Project Documentation](https://github.com/ludoux/esp32-nut-server-usbhid)

---

## 🙏 Credits

**Original Author**: [ludoux](https://github.com/ludoux) - Created the base ESP32 USB-HID NUT server implementation

**This Fork**: Specialized for CyberPower UPS integration with Home Assistant, featuring improved reliability, accuracy, and maintainability.

---

## 📄 License

GPL-3.0 (inherited from original project)

---

**Note**: This is a development project. Use at your own risk in production environments.

For detailed technical information, video demos, and original implementation details, please visit the [original repository](https://github.com/ludoux/esp32-nut-server-usbhid).
