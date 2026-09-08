# Linux for Honor Magicbook Art 14 with Snapdragon Elite

## About

Linux kernel based on Linaro and QCOM work with patches, mainly dtb, in order to boot on Honor Magicbook Art 14 laptop.
This is the also based on the Ubuntu resolute mainline kernel.

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
  - [x] DSC Mode - based on patches from https://github.com/xzn/linux_msm.git, mostly works
- [x] Display with Alt DP - works, tested with multiple Type C adapters
- [x] GPU - working fine when external monitor connected, needs firmware extracted from Windows
- [x] HDMI port on the right side, working with dispcc enabled, same as AltDP
- [x] Sleep - seems to work, even with the EC, see sleep notes
- [ ] Hibernate - Doesn't work, seems some drivers are not handling this correctly
- [x] Iris Video codec - needs firmware extracted from linux, works
- [x] EL2 boot - the kernel boots with slbounce provided the el2 dtb is loaded, unstable


## EC Notes

The EC driver enables all the special keys, modern sleep and extra sensors:
* Some extra thermal sensors
* Fans RPM reading
* Battery nominal voltage

## Sleep notes

Qualcomm describes the MSM sleep states here:  https://docs.qualcomm.com/doc/80-80022-30/topic/qcs6490-soc-power-saving-state.html

Right now the sleep states are as follows:
```
cat /sys/kernel/debug/qcom_stats/{cxsd,aosd,apss,ddr}                                                                                                  [⏱ 2s]
Count: 0
Last Entered At: 0
Last Exited At: 0
Accumulated Duration: 0
Count: 0
Last Entered At: 0
Last Exited At: 0
Accumulated Duration: 0
Count: 1
Last Entered At: 21719807724
Last Exited At: 21887257142
Accumulated Duration: 167449418
Count: 0
Last Entered At: 0
Last Exited At: 0
Accumulated Duration: 0
```
