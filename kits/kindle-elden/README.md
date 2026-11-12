# Elden Service for Kindle

Install the extension under `/mnt/us/extensions/elden-tools`. Reopen KUAL and
choose **Elden Service**. Each command writes its result to
`/mnt/us/extensions/elden-tools/status.txt` and prints the same output when
called from Terminal or SSH.

The Kindle needs to be the USB host. Connect a USB-C OTG adapter to the
Kindle and use a data cable from the adapter to the tablet. Run **Status** to
check the USB host bus and, when the tablet is in bootloader fastboot, its
current slot. The observed Paperwhite kernel has no CDC ACM driver, so the
collector reads the Elden USB console using libusb.

- **Start logs and WireGuard** starts the collector. It reconnects when the
  tablet enumerates its USB console and keeps the latest two 16 MiB logs.
- **Activate tablet slot A/B** selects and verifies a slot in bootloader
  fastboot. **Reboot tablet from fastboot** is a separate action.
- **Restore Linux to tablet boot_b** writes the original, checksum-pinned
  96 MiB backup to `boot_b`. The active slot is reported afterward. To boot
  the restored image, activate B and then reboot.

The log reader binds to `10.8.0.8:8734` over WireGuard. From the LAN peer
`10.8.0.1` run `curl -f http://10.8.0.8:8734/latest`. The previous log is
available at `/previous` after rotation. The Kindle also retains both files
under `/var/local/elden-tools/logs`.

## Provisioning

Use Kindle root SSH to stream the owner-provided WireGuard profile and the
original slot B backup to the extension's staging script. Both files go into
private Kindle storage.

```sh
ssh -p 2222 root@KINDLE /mnt/us/extensions/elden-tools/bin/stage.sh wireguard < ~/wg-kindle.conf
ssh -p 2222 root@KINDLE /mnt/us/extensions/elden-tools/bin/stage.sh backup < /path/to/partitions/boot_b.img
```

## Build

Cross-compile the USB reader with Debian ARMhf libusb headers and libraries.
Extract the Debian ARMhf `fastboot` package and its dependencies into
`$DEBIAN_SYSROOT`. Point `$WG_RUNTIME` at the Kindle WireGuard bundle with
`wireguard-go`, `wg-arm`, and its musl runtime under `lib/`.

```sh
arm-linux-gnueabihf-gcc -std=c11 -Wall -Wextra -Werror -O2 -o usb-cdc usb-cdc.c -lusb-1.0
(cd collector && CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -trimpath -o ../collector-arm .)
python3 package.py --sysroot "$DEBIAN_SYSROOT" --usb-reader usb-cdc --collector collector-arm \
    --wireguard "$WG_RUNTIME" elden-kindle.zip
```

Use the Android fastboot CLI from the [Debian Android platform tools package](https://packages.debian.org/trixie/fastboot).
The VPN runtime uses [WireGuard userspace tools](https://www.wireguard.com/repositories/).
