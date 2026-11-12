package org.unixnu.eldensvc;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.IBinder;
import android.os.PowerManager;

import java.io.IOException;

public final class CaptureService extends Service {
    static volatile String status = "USB capture stopped";
    private volatile boolean running;
    private Thread worker;
    private PowerManager.WakeLock wakeLock;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (worker != null) return START_NOT_STICKY;
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
        running = true;
        worker = new Thread(this::capture, "elden-usb");
        worker.start();
        return START_NOT_STICKY;
    }

    private void capture() {
        UsbManager manager = getSystemService(UsbManager.class);
        String requested = "";
        try {
            BootLog.marker(this, "Waiting for a USB CDC console");
            while (running) {
                UsbDevice device = UsbLink.find(manager, false);
                if (device == null) {
                    status = "Waiting for the tablet’s USB console";
                    requested = "";
                } else if (!manager.hasPermission(device)) {
                    status = "Grant Android USB permission to capture";
                    if (!requested.equals(device.getDeviceName())) {
                        requested = device.getDeviceName();
                        UsbLink.requestPermission(this, manager, device, 2);
                    }
                } else {
                    try (UsbLink link = UsbLink.open(manager, device, false)) {
                        BootLog.marker(this, "USB CDC console connected");
                        status = "Recording the tablet USB console";
                        byte[] buffer = new byte[8192];
                        while (running && device.equals(manager.getDeviceList().get(device.getDeviceName()))) {
                            int count = link.receive(buffer, 750);
                            if (count > 0) BootLog.append(this, buffer, count);
                        }
                        BootLog.marker(this, "USB CDC console disconnected");
                    } catch (IOException | RuntimeException error) {
                        status = "USB console unavailable. " + error.getMessage();
                    }
                }
                Thread.sleep(500);
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        } catch (IOException | RuntimeException error) {
            status = "USB capture failed. " + error.getMessage();
            stopSelf();
        }
    }

    @Override
    public void onDestroy() {
        running = false;
        if (worker != null) worker.interrupt();
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        if (!status.startsWith("USB capture failed")) status = "USB capture stopped";
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
