package org.unixnu.eldensvc;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;

import java.io.Closeable;
import java.io.IOException;
import java.util.Collection;
import java.util.Locale;
import java.util.TreeSet;

final class UsbLink implements Closeable, Fastboot.Transport {
    static final String PERMISSION = "org.unixnu.eldensvc.USB_PERMISSION";
    private final UsbDeviceConnection connection;
    private final UsbInterface control;
    private final UsbInterface data;
    private final UsbEndpoint input;
    private final UsbEndpoint output;

    private UsbLink(UsbDeviceConnection connection, UsbInterface control, UsbInterface data,
                    UsbEndpoint input, UsbEndpoint output) {
        this.connection = connection;
        this.control = control;
        this.data = data;
        this.input = input;
        this.output = output;
    }

    private static UsbInterface fastboot(UsbDevice device) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface candidate = device.getInterface(i);
            if (candidate.getInterfaceClass() == 255 && candidate.getInterfaceSubclass() == 66
                    && candidate.getInterfaceProtocol() == 3) return candidate;
        }
        return null;
    }

    private static UsbInterface serialControl(UsbDevice device) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface candidate = device.getInterface(i);
            if (candidate.getInterfaceClass() == UsbConstants.USB_CLASS_COMM
                    && candidate.getInterfaceSubclass() == 2) return candidate;
        }
        return null;
    }

    private static UsbEndpoint endpoint(UsbInterface data, int direction) {
        for (int i = 0; i < data.getEndpointCount(); i++) {
            UsbEndpoint candidate = data.getEndpoint(i);
            if (candidate.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK
                    && candidate.getDirection() == direction) return candidate;
        }
        return null;
    }

    private static boolean hasSerialEndpoints(UsbInterface candidate) {
        return candidate.getInterfaceClass() == UsbConstants.USB_CLASS_CDC_DATA
                && endpoint(candidate, UsbConstants.USB_DIR_IN) != null
                && endpoint(candidate, UsbConstants.USB_DIR_OUT) != null;
    }

    private static UsbInterface serialData(UsbDevice device, UsbInterface control, int unionDataId) {
        UsbInterface fallback = null;
        int candidates = 0;
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface candidate = device.getInterface(i);
            if (!hasSerialEndpoints(candidate)) continue;
            if (unionDataId >= 0) {
                if (candidate.getId() == unionDataId) return candidate;
            } else if (candidate.getInterfaceProtocol() == 0) {
                if (candidate.getId() == control.getId() + 1) return candidate;
                fallback = candidate;
                candidates++;
            }
        }
        return candidates == 1 ? fallback : null;
    }

    private static boolean hasSerialFunction(UsbDevice device) {
        if (serialControl(device) == null) return false;
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            if (hasSerialEndpoints(device.getInterface(i))) return true;
        }
        return false;
    }

    static UsbDevice find(UsbManager manager, boolean forFastboot) throws IOException {
        return find(manager.getDeviceList().values(), forFastboot);
    }

    static UsbDevice find(Collection<UsbDevice> devices, boolean forFastboot) throws IOException {
        UsbDevice match = null;
        UsbDevice elden = null;
        int matches = 0;
        for (UsbDevice device : devices) {
            boolean suitable = forFastboot ? fastboot(device) != null : hasSerialFunction(device);
            if (!suitable) continue;
            match = device;
            matches++;
            if (!forFastboot && eldenConsole(device)) {
                if (elden != null) throw new IOException("More than one Elden USB console is connected");
                elden = device;
            }
        }
        if (elden != null) return elden;
        if (matches > 1) throw new IOException("More than one matching USB device is connected");
        return match;
    }

    static boolean eldenConsole(UsbDevice device) {
        return device.getVendorId() == 0x1209 && device.getProductId() == 0x7004;
    }

    static UsbDevice intentDevice(Intent intent) {
        return Build.VERSION.SDK_INT >= 33
                ? intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class)
                : intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
    }

    static String describe(Collection<UsbDevice> devices) {
        TreeSet<String> descriptors = new TreeSet<>();
        for (UsbDevice device : devices) {
            String mode = fastboot(device) != null ? "fastboot"
                    : hasSerialFunction(device) ? "CDC ACM"
                    : eldenConsole(device) ? "Elden device without a usable CDC interface" : "other";
            descriptors.add(String.format(Locale.ROOT, "%04x:%04x (%s)",
                    device.getVendorId(), device.getProductId(), mode));
        }
        return descriptors.isEmpty() ? "no devices visible" : String.join(", ", descriptors);
    }

    static void requestPermission(Context context, UsbManager manager, UsbDevice device, int request) {
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 31) flags |= PendingIntent.FLAG_MUTABLE;
        Intent intent = new Intent(PERMISSION).setPackage(context.getPackageName());
        manager.requestPermission(device, PendingIntent.getBroadcast(context, request, intent, flags));
    }

    static UsbLink open(UsbManager manager, UsbDevice device, boolean forFastboot) throws IOException {
        UsbInterface control = forFastboot ? null : serialControl(device);
        if (!forFastboot && control == null) throw new IOException("USB serial control interface disappeared");
        UsbDeviceConnection connection = manager.openDevice(device);
        if (connection == null) throw new IOException("Android could not open the USB device");
        UsbLink link = null;
        try {
            UsbInterface data = forFastboot ? fastboot(device) : serialData(device, control,
                    CdcDescriptors.dataInterface(connection.getRawDescriptors(), control.getId()));
            if (data == null) throw new IOException("USB data interface is unavailable");
            UsbEndpoint input = endpoint(data, UsbConstants.USB_DIR_IN);
            UsbEndpoint output = endpoint(data, UsbConstants.USB_DIR_OUT);
            if (input == null || output == null) throw new IOException("USB bulk endpoints are unavailable");
            link = new UsbLink(connection, control, data, input, output);
            if ((control != null && !connection.claimInterface(control, true))
                    || !connection.claimInterface(data, true)) {
                throw new IOException("Android could not claim the USB interface");
            }
            if (data.getAlternateSetting() != 0 && !connection.setInterface(data)) {
                throw new IOException("Android could not select the USB data interface");
            }
            if (control != null) {
                byte[] lineCoding = {0, (byte) 0xc2, 1, 0, 0, 0, 8};
                int result = connection.controlTransfer(0x21, 0x20, 0, control.getId(),
                        lineCoding, lineCoding.length, 1000);
                if (result != lineCoding.length) {
                    throw new IOException("CDC SET_LINE_CODING returned " + result);
                }
                result = connection.controlTransfer(0x21, 0x22, 3, control.getId(),
                        null, 0, 1000);
                if (result != 0) {
                    throw new IOException("CDC SET_CONTROL_LINE_STATE returned " + result);
                }
            }
            return link;
        } catch (IOException | RuntimeException error) {
            if (link != null) link.close();
            else connection.close();
            throw error;
        }
    }

    @Override
    public void send(byte[] bytes, int length, int timeout) throws IOException {
        int sent = connection.bulkTransfer(output, bytes, length, timeout);
        if (sent != length) throw new IOException("USB write was incomplete");
    }

    @Override
    public int receive(byte[] bytes, int timeout) {
        return connection.bulkTransfer(input, bytes, bytes.length, timeout);
    }

    @Override
    public void close() {
        connection.releaseInterface(data);
        if (control != null) connection.releaseInterface(control);
        connection.close();
    }
}
