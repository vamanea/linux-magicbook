# Linux for Honor Magicbook Art 14 with Snapdragon Elite

## About

Linux kernel based on Linaro work from https://gitlab.com/Linaro/arm64-laptops/linux.git with patches, mainly for DTB to boot on Honor Magicbook Art 14 laptop.
 - Also included are the patches from here: https://github.com/xzn/linux_msm which in turn are based on original qualcomm work that seems to have stalled: https://lore.kernel.org/lkml/1674498274-6010-1-git-send-email-quic_khsieh@quicinc.com
[1] https://github.com/ROCKNIX/distribution/pull/2716
[2] https://github.com/ROCKNIX/distribution/pull/2717

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
- [x] Display - module probes fine, DSC support is needed(patches integrated), needs firmware extracted from Windows
  - [x] OLED Panel - some artefacts on boot but once the mode switch to DM happens all works(60Hz and 120Hz both supported)
- [x] Display with Alt DP - works, tested with multiple Type C adapters
- [x] GPU - works, needs firmware extracted from Windows
- [x] HDMI port on the right side, working with dispcc enabled, same as AltDP
- [ ] Sleep - device reboots
- [ ] Hibernate - not tested
- [x] Iris Video codec - needs firmware extracted from linux, works, AV1 patch is not yet merged
- [x] EL2 boot - the kernel boots with slbounce provided the el2 dtb is loaded, unstable
 
