# Linux for Honor Magicbook Art 14 with Snapdragon Elite

## About

Linux kernel based on Linaro work from https://git.codelinaro.org/abel.vesa/linux with patches, mainly dtb, in order to boot on Honor Magicbook Art 14 laptop.

## Status    
Supported features:
- [x] Keyboard
- [x] Touchpad - needs quirks for libinput
- [x] Touchscreen
- [x] PCIe ports (pcie4)
  - [x] Wifi (WCN7850) - needs firmware extracted from Windows
  - [x] BT (WCN7850) - needs firmware extracted from Windows
- [x] USB type-c, type-a, Magnetic webcam connector
- [x] ADSP and CDSP - needs firmware extracted from Windows
- [x] UFS storage - power management generates some kernel warnings but nothing fatal
- [ ] Audio - needs firmware extracted from Windows and topology files
- [ ] Display - module probes fine, but causes the screen to become green, use simpledrm instead, needs firmware extracted from Windows
- [x] Display with Alt DP - works, tested with multiple Type C adapters
- [x] GPU - working fine when external monitor connected, needs firmware extracted from Windows
- [ ] HDMI port on the right side, not working
- [ ] Sleep - device reboots
- [ ] Hibernate - not tested
 
