package org.unixnu.eldensvc;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.net.VpnService;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MainActivity extends Activity {
    private static final int REQUEST_VPN = 100;
    private static final int REQUEST_BACKUP = 101;
    private static final int INK = 0xff16232d;
    private static final int MUTED = 0xff455b6a;
    private static final int ACCENT = 0xff124b60;
    private static final ExecutorService WORK = Executors.newSingleThreadExecutor();
    private static final AtomicBoolean BUSY = new AtomicBoolean();
    private static volatile String actionStatus = "";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private UsbManager usb;
    private WireGuard wireGuard;
    private UsbDevice pendingUsb;
    private String pendingAction;
    private TextView usbStatus;
    private TextView operation;
    private TextView capture;
    private TextView vpn;
    private TextView log;
    private Button restore;
    private boolean receiverRegistered;

    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            refresh();
            handler.postDelayed(this, 800);
        }
    };

    private final BroadcastReceiver permission = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            if (pendingUsb == null) return;
            UsbDevice device = UsbLink.intentDevice(intent);
            if (!pendingUsb.equals(device)) return;
            String action = pendingAction;
            pendingUsb = null;
            pendingAction = null;
            if (usb.hasPermission(device)) runFastboot(device, action);
            else setStatus("Android USB permission was declined");
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        usb = getSystemService(UsbManager.class);
        wireGuard = WireGuard.get(this);
        buildView();
        IntentFilter filter = new IntentFilter(UsbLink.PERMISSION);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(permission, filter, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(permission, filter);
        receiverRegistered = true;
        WORK.execute(wireGuard::checkProfile);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Intent intent = getIntent();
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) {
            UsbDevice attached = UsbLink.intentDevice(intent);
            setIntent(new Intent(Intent.ACTION_MAIN));
            if (attached != null) {
                UsbDevice live = usb.getDeviceList().get(attached.getDeviceName());
                if (live != null && UsbLink.eldenConsole(live) && usb.hasPermission(live)) {
                    startCapture();
                }
            }
        }
        handler.post(refresh);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refresh);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (receiverRegistered) unregisterReceiver(permission);
        super.onDestroy();
    }

    private int dp(int number) {
        return Math.round(number * getResources().getDisplayMetrics().density);
    }

    private TextView text(LinearLayout parent, String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        if (bold) view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        parent.addView(view, new LinearLayout.LayoutParams(-1, -2));
        return view;
    }

    private GradientDrawable rounded(int color, int radius) {
        GradientDrawable background = new GradientDrawable();
        background.setColor(color);
        background.setCornerRadius(dp(radius));
        return background;
    }

    private LinearLayout section(LinearLayout parent, String title) {
        LinearLayout group = new LinearLayout(this);
        group.setOrientation(LinearLayout.VERTICAL);
        group.setPadding(dp(16), dp(14), dp(16), dp(12));
        group.setBackground(rounded(Color.WHITE, 14));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(12);
        parent.addView(group, params);
        text(group, title, 18, INK, true).setPadding(0, 0, 0, dp(6));
        return group;
    }

    private Button button(LinearLayout parent, String label, Runnable run) {
        Button view = new Button(this);
        view.setAllCaps(false);
        view.setText(label);
        view.setTextColor(ACCENT);
        view.setMinHeight(dp(46));
        view.setOnClickListener(clicked -> run.run());
        boolean row = parent.getOrientation() == LinearLayout.HORIZONTAL;
        parent.addView(view, new LinearLayout.LayoutParams(row ? 0 : -1, -2, row ? 1 : 0));
        return view;
    }

    private LinearLayout row(LinearLayout parent) {
        LinearLayout row = new LinearLayout(this);
        parent.addView(row, new LinearLayout.LayoutParams(-1, -2));
        return row;
    }

    private void buildView() {
        getWindow().setStatusBarColor(0xfff3f6f8);
        getWindow().setNavigationBarColor(0xfff3f6f8);
        getWindow().getDecorView().setSystemUiVisibility(
                android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR
                        | android.view.View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(0xfff3f6f8);
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(16), dp(16), dp(16), dp(20));
        scroll.addView(body, new ViewGroup.LayoutParams(-1, -2));
        setContentView(scroll);

        text(body, "Elden Service", 30, INK, true);
        usbStatus = text(body, "Checking USB", 15, MUTED, false);

        LinearLayout fastboot = section(body, "Bootloader fastboot");
        button(fastboot, "Read active slot", () -> fastboot("status"));
        LinearLayout slots = row(fastboot);
        button(slots, "Activate slot A", () -> fastboot("a"));
        button(slots, "Activate slot B", () -> fastboot("b"));
        button(fastboot, "Select Linux backup", this::selectBackup);
        restore = button(fastboot, "Restore Linux to boot_b", () -> fastboot("restore"));
        button(fastboot, "Reboot tablet", () -> fastboot("reboot"));
        operation = text(fastboot, "", 14, MUTED, false);

        LinearLayout logs = section(body, "USB boot log");
        capture = text(logs, "", 14, MUTED, false);
        LinearLayout captureActions = row(logs);
        button(captureActions, "Start capture", this::startCapture);
        button(captureActions, "Stop capture",
                () -> stopService(new Intent(this, CaptureService.class)));
        button(logs, "Clear logs", () -> task(() -> {
            BootLog.clear(this);
            runOnUiThread(() -> {
                refresh();
                Toast.makeText(this, "Boot logs cleared", Toast.LENGTH_SHORT).show();
            });
        }));

        LinearLayout network = section(body, "WireGuard");
        vpn = text(network, "", 14, MUTED, false);
        LinearLayout vpnActions = row(network);
        button(vpnActions, "Connect", this::requestVpn);
        button(vpnActions, "Disconnect", () -> task(() -> wireGuard.stop()));

        log = text(logs, "", 12, Color.WHITE, false);
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextIsSelectable(true);
        log.setPadding(dp(10), dp(10), dp(10), dp(10));
        log.setBackground(rounded(0xff233743, 8));
        refresh();
    }

    private void startCapture() {
        startForegroundService(new Intent(this, CaptureService.class));
    }

    private void refresh() {
        try {
            String connection = UsbLink.find(usb, true) != null ? "Fastboot device connected"
                    : UsbLink.find(usb, false) != null ? "USB serial console connected"
                    : "Waiting for a tablet USB connection";
            usbStatus.setText(connection);
        } catch (IOException error) {
            usbStatus.setText(error.getMessage());
        }
        operation.setText(actionStatus);
        restore.setEnabled(backup().isFile() && !BUSY.get());
        capture.setText(CaptureService.status);
        vpn.setText(wireGuard.status());
        String tail = BootLog.tail(this);
        if (tail.isEmpty()) tail = "No USB log captured yet";
        if (!tail.contentEquals(log.getText())) log.setText(tail);
    }

    private void setStatus(String text) {
        actionStatus = text;
        operation.setText(text);
    }

    interface Work {
        void run() throws Exception;
    }

    private void task(Work operation) {
        if (!BUSY.compareAndSet(false, true)) {
            setStatus("Another operation is running");
            return;
        }
        WORK.execute(() -> {
            try {
                operation.run();
            } catch (Exception error) {
                actionStatus = error.getMessage() != null ? error.getMessage() : "Operation failed";
            } finally {
                BUSY.set(false);
            }
        });
    }

    private void fastboot(String action) {
        if (BUSY.get() || pendingUsb != null) {
            setStatus("Another operation is running");
            return;
        }
        try {
            UsbDevice device = UsbLink.find(usb, true);
            if (device == null) throw new IOException("Waiting for the tablet in bootloader fastboot");
            if (usb.hasPermission(device)) {
                runFastboot(device, action);
            } else {
                pendingUsb = device;
                pendingAction = action;
                UsbLink.requestPermission(this, usb, device, 1);
            }
        } catch (IOException | RuntimeException error) {
            pendingUsb = null;
            pendingAction = null;
            setStatus(error.getMessage());
        }
    }

    private void runFastboot(UsbDevice device, String action) {
        setStatus("Checking the tablet");
        task(() -> {
            try (UsbLink link = UsbLink.open(usb, device, true)) {
                Fastboot fastboot = new Fastboot(link, detail -> actionStatus = detail);
                fastboot.verifyTablet();
                switch (action) {
                    case "status" -> actionStatus = "Active slot " + fastboot.slot().toUpperCase(Locale.ROOT);
                    case "a", "b" -> {
                        fastboot.activate(action);
                        actionStatus = "Active slot " + action.toUpperCase(Locale.ROOT);
                    }
                    case "reboot" -> {
                        fastboot.reboot();
                        actionStatus = "Reboot accepted";
                    }
                    case "restore" -> {
                        actionStatus = "Verifying the Linux backup";
                        fastboot.restore(backup());
                        actionStatus = "Linux restored to boot_b";
                    }
                    default -> throw new IOException("Unknown operation");
                }
            }
        });
    }

    private File backup() {
        return new File(getFilesDir(), "boot_b-linux.img");
    }

    private void selectBackup() {
        Intent select = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        select.addCategory(Intent.CATEGORY_OPENABLE);
        select.setType("*/*");
        startActivityForResult(select, REQUEST_BACKUP);
    }

    private void importBackup(Uri uri) throws IOException {
        File staging = File.createTempFile("linux-backup", ".img", getFilesDir());
        try {
            try (InputStream input = getContentResolver().openInputStream(uri);
                 FileOutputStream output = new FileOutputStream(staging)) {
                if (input == null) throw new IOException("Could not open the backup");
                byte[] buffer = new byte[64 * 1024];
                long written = 0;
                int count;
                while ((count = input.read(buffer)) != -1) {
                    written += count;
                    if (written > Fastboot.BACKUP_SIZE) throw new IOException("The Linux backup must be 96 MiB");
                    output.write(buffer, 0, count);
                }
            }
            Fastboot.verifyBackup(staging);
            if (!staging.renameTo(backup())) throw new IOException("Could not save the Linux backup");
            actionStatus = "Linux backup verified and staged";
        } finally {
            if (staging.exists()) staging.delete();
        }
    }

    private void requestVpn() {
        if (!WireGuard.configured(this)) {
            setStatus("WireGuard profile is missing");
            return;
        }
        Intent permission = VpnService.prepare(this);
        if (permission == null) task(() -> wireGuard.start());
        else startActivityForResult(permission, REQUEST_VPN);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK) return;
        if (request == REQUEST_VPN) task(() -> wireGuard.start());
        if (request == REQUEST_BACKUP && data != null && data.getData() != null) {
            Uri uri = data.getData();
            task(() -> importBackup(uri));
        }
    }
}
