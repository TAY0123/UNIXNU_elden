package org.unixnu.eldensvc;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

import java.io.IOException;
import java.util.Collection;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;

public final class CaptureService extends Service {
    static volatile String status = "USB capture stopped";
    private volatile boolean running;
    private Thread worker;
    private PowerManager.WakeLock wakeLock;
    private final ArrayBlockingQueue<Boolean> usbChanges = new ArrayBlockingQueue<>(1);
    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            usbChanges.offer(true);
        }
    };

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (worker != null) {
            usbChanges.offer(true);
            return START_NOT_STICKY;
        }
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(new NotificationChannel("capture", "Elden USB capture",
                NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        startForeground(1, new Notification.Builder(this, "capture")
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle("Elden USB capture")
                .setContentText("Waiting for or recording the tablet console")
                .setContentIntent(open).setOngoing(true).build());
        wakeLock = getSystemService(PowerManager.class).newWakeLock(
                PowerManager.PARTIAL_WAKE_LOCK, "eldensvc:usb-capture");
        wakeLock.acquire();
        IntentFilter filter = new IntentFilter(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        filter.addAction(UsbLink.PERMISSION);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(usbReceiver, filter);
        running = true;
        worker = new Thread(this::capture, "elden-usb");
        worker.start();
        return START_NOT_STICKY;
    }

    private void capture() {
        UsbManager manager = getSystemService(UsbManager.class);
        String requested = "";
        String lastDevices = "";
        try {
            report("Waiting for the tablet’s USB console");
            while (running) {
                Collection<UsbDevice> devices = manager.getDeviceList().values();
                String description = UsbLink.describe(devices);
                if (!description.equals(lastDevices)) {
                    BootLog.marker(this, "Android USB host: " + description);
                    lastDevices = description;
                }
                UsbDevice device;
                try {
                    device = UsbLink.find(devices, false);
                } catch (IOException error) {
                    report(error.getMessage());
                    usbChanges.poll(500, TimeUnit.MILLISECONDS);
                    continue;
                }
                if (device == null) {
                    report("Waiting for the tablet’s USB console");
                    requested = "";
                } else if (!manager.hasPermission(device)) {
                    report("Grant Android USB permission to capture");
                    if (!requested.equals(device.getDeviceName())) {
                        requested = device.getDeviceName();
                        UsbLink.requestPermission(this, manager, device, 2);
                    }
                } else {
                    try (UsbLink link = UsbLink.open(manager, device, false)) {
                        report("USB CDC console connected, waiting for bytes");
                        byte[] buffer = new byte[8192];
                        while (running) {
                            Map<String, UsbDevice> attached = manager.getDeviceList();
                            if (!device.equals(attached.get(device.getDeviceName()))) break;
                            if (usbChanges.poll() != null && !UsbLink.eldenConsole(device)
                                    && attached.values().stream().anyMatch(UsbLink::eldenConsole)) break;
                            int count = link.receive(buffer, 750);
                            if (count > 0) {
                                report("Recording the tablet USB console");
                                BootLog.append(this, buffer, count);
                            }
                        }
                        report("USB CDC console disconnected");
                    } catch (IOException | RuntimeException error) {
                        report("USB console unavailable. " + error.getMessage());
                    }
                }
                usbChanges.poll(500, TimeUnit.MILLISECONDS);
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        } catch (IOException | RuntimeException error) {
            status = "USB capture failed. " + error.getMessage();
            stopSelf();
        }
    }

    private void report(String next) throws IOException {
        if (!next.equals(status)) {
            BootLog.marker(this, next);
            status = next;
        }
    }

    @Override
    public void onDestroy() {
        running = false;
        if (worker != null) {
            unregisterReceiver(usbReceiver);
            worker.interrupt();
        }
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        if (!status.startsWith("USB capture failed")) status = "USB capture stopped";
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
