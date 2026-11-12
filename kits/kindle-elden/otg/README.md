# Kindle Paperwhite USB host probe

The Paperwhite Bellatrix running `5.15.41-lab126` declares an OTG-capable
MTU3 controller. Its cable detector reports incoming VBUS, and its xHCI
child advertises `mediatek,mt8512-xhci`, which the stock driver does not
match. The device tree supplies an `xhci_ck` clock which the parent driver
looks up under a different name.

The probe takes a clock reference and signals host mode through the stock
extcon driver. `host-mode.sh` binds `xhci-mtk` temporarily. Stop through
the script so it unbinds xHCI before dropping the clock or restoring the
firmware role.

## Build and run

Use the Kindle's `/proc/config.gz` and its stock
`kernel/drivers/usb/gadget/function/usb_f_acm.ko` as ABI inputs.
Extract `gplrelease/linux-5.15.tar.gz` from the matching
[Amazon source archive](https://s3.amazonaws.com/kindledownloads/Kindle_src_5.17.1.0.4_4351970007.tar.gz).
That tree references a missing `drivers/misc/mediatek/cfa/Kconfig`. In the
private build tree, create that file with a tristate declaration named
`MTK_CFA` to preserve the live config during Kbuild.

```sh
sh build.sh /path/to/linux live.config usb_f_acm.ko build
```

Copy the module and script to the Kindle. With USB disconnected and Wi-Fi
active, run the following as Kindle root.

```sh
sh host-mode.sh start ./elden_kindle_otg.ko
sh host-mode.sh status
sh host-mode.sh stop
```

On the tested firmware this sequence registered the USB 2 and USB 3 root
hubs. The stock device tree supplies no host VBUS regulator. External
peripheral enumeration and boot-log capture need separate hardware checks.
