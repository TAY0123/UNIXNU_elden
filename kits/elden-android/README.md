# Elden Service for Android

Elden Service uses Android USB host mode to read the tablet's active slot,
activate slot A or B, reboot from bootloader fastboot, and restore the
verified 96 MiB Linux backup to `boot_b`. It keeps the active-slot change
and reboot as separate actions. USB capture reads CDC ACM from a Linux
serial gadget or the UniXNU Recovery console and retains two 16 MiB logs.

Connect the Android device as the USB host and grant access when prompted.
Use **Start capture** to collect USB logs across tablet USB disconnects and
reconnections. Activate **WireGuard → Connect** to serve the log to the
`10.8.0.1` LAN peer at `http://10.8.0.8:8734/latest`. The previous log is
served at `/previous` after rotation. **Clear logs** deletes the previous
log and empties the current log while capture continues.

Leave capture running when moving from fastboot to the UniXNU menu and
then into **Elden macOS 26.5 - Recovery**. Android may ask for USB access
as the tablet re-enumerates. If Android opens Elden Service for the
Elden console, capture starts automatically. The log records the USB vendor and product
IDs Android sees, whether a CDC session opened, and when its first bytes
arrived.

The app uses the Kindle profile's identity and endpoint, with a route for
`10.8.0.0/24` and a 25-second keepalive for inbound log access. Stop the
Kindle tunnel before activating this same peer on Android.

## Build and install

Use JDK 17, Gradle 8.11.1, Android SDK 35, and Python `cryptography`.
Run from this directory with a new private staging directory.

```sh
python3 seal-profile.py ~/wg-kindle.conf /path/to/private/assets /path/to/private/wireguard.key
gradle -PeldenPrivateAssets=/path/to/private/assets assembleDebug
adb install -r build/outputs/apk/debug/EldenService-debug.apk
adb shell -T "run-as org.unixnu.eldensvc sh -c 'umask 077; mkdir -p files; cat > files/wireguard.key'" \
    < /path/to/private/wireguard.key
```

The APK contains the sealed profile. Provision the key to the app's
private storage over an authorized ADB session. The on-device profile
check decrypts and parses the asset before connecting. Keep the APK
staging and provisioning key outside the source tree.

The **Select Linux backup** action copies the original verified
`partitions/boot_b.img` into app-private storage. Restore rechecks its
SHA-256 and the target partition size before sending the image.
