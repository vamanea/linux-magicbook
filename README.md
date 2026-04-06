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
  - [x] BT (WCN7850) - needs firmware extracted from Windows(mainline firmware doesn't work properly)
- [x] USB type-c, type-a, Magnetic webcam connector
- [x] ADSP and CDSP - needs firmware extracted from Windows
- [x] UFS storage - power management generates some kernel warnings but nothing fatal
- [x] Audio - needs firmware extracted from Windows and topology files, all speakers work
- [x] Display - module probes fine, but causes the screen to become gray, use simpledrm instead, needs firmware extracted from Windows
  - [x] OLED Panel - the panel needs DSC disable to work, msm drm driver doesn't yet support it
- [x] Display with Alt DP - works, tested with multiple Type C adapters
- [x] GPU - working fine when external monitor connected, needs firmware extracted from Windows
- [x] HDMI port on the right side, working with dispcc enabled, same as AltDP
- [x] Sleep - seems to work
- [ ] Hibernate - not tested
- [x] Iris Video codec - needs firmware extracted from linux, works
- [x] EL2 boot - the kernel boots with slbounce provided the el2 dtb is loaded, unstable
 
