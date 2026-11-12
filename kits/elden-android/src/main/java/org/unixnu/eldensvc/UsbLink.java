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

    private static UsbInterface serialData(UsbDevice device) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface candidate = device.getInterface(i);
            if (candidate.getInterfaceClass() == UsbConstants.USB_CLASS_CDC_DATA
                    && endpoint(candidate, UsbConstants.USB_DIR_IN) != null) return candidate;
        }
        return null;
    }

    static UsbDevice find(UsbManager manager, boolean forFastboot) throws IOException {
        UsbDevice match = null;
        for (UsbDevice device : manager.getDeviceList().values()) {
            boolean suitable = forFastboot ? fastboot(device) != null
                    : serialControl(device) != null && serialData(device) != null;
            if (!suitable) continue;
            if (match != null) throw new IOException("More than one matching USB device is connected");
            match = device;
        }
        return match;
    }

    static void requestPermission(Context context, UsbManager manager, UsbDevice device, int request) {
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 31) flags |= PendingIntent.FLAG_MUTABLE;
        Intent intent = new Intent(PERMISSION).setPackage(context.getPackageName());
        manager.requestPermission(device, PendingIntent.getBroadcast(context, request, intent, flags));
    }

    static UsbLink open(UsbManager manager, UsbDevice device, boolean forFastboot) throws IOException {
        UsbInterface control = forFastboot ? null : serialControl(device);
        UsbInterface data = forFastboot ? fastboot(device) : serialData(device);
        if (data == null || (!forFastboot && control == null)) throw new IOException("USB interface disappeared");
        UsbEndpoint input = endpoint(data, UsbConstants.USB_DIR_IN);
        UsbEndpoint output = endpoint(data, UsbConstants.USB_DIR_OUT);
        if (input == null || output == null) throw new IOException("USB bulk endpoints are unavailable");
        UsbDeviceConnection connection = manager.openDevice(device);
        if (connection == null) throw new IOException("Android could not open the USB device");
        UsbLink link = new UsbLink(connection, control, data, input, output);
        try {
            if ((control != null && !connection.claimInterface(control, true))
                    || !connection.claimInterface(data, true)) {
                throw new IOException("Android could not claim the USB interface");
            }
            if (data.getAlternateSetting() != 0 && !connection.setInterface(data)) {
                throw new IOException("Android could not select the USB data interface");
            }
            if (control != null) {
                byte[] lineCoding = {0, (byte) 0xc2, 1, 0, 0, 0, 8};
                if (connection.controlTransfer(0x21, 0x20, 0, control.getId(),
                        lineCoding, lineCoding.length, 1000) != lineCoding.length
                        || connection.controlTransfer(0x21, 0x22, 3, control.getId(),
                        null, 0, 1000) != 0) {
                    throw new IOException("The tablet did not accept CDC serial setup");
                }
            }
            return link;
        } catch (IOException | RuntimeException error) {
            link.close();
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
