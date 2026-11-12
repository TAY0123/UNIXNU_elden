package org.unixnu.eldensvc;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Instant;

final class BootLog {
    private static final int LIMIT = 16 * 1024 * 1024;
    private static final Object LOCK = new Object();

    static File file(Context context, boolean previous) {
        return new File(context.getFilesDir(), previous ? "previous.log" : "latest.log");
    }

    static void marker(Context context, String message) throws IOException {
        byte[] bytes = ("\n[" + Instant.now() + "] " + message + "\n").getBytes(StandardCharsets.UTF_8);
        append(context, bytes, bytes.length);
    }

    static void append(Context context, byte[] bytes, int length) throws IOException {
        synchronized (LOCK) {
            File latest = file(context, false);
            if (latest.length() + length > LIMIT && !latest.renameTo(file(context, true))) {
                throw new IOException("Could not rotate the boot log");
            }
            try (FileOutputStream stream = new FileOutputStream(latest, true)) {
                stream.write(bytes, 0, length);
            }
        }
    }

    static void clear(Context context) throws IOException {
        synchronized (LOCK) {
            File empty = File.createTempFile("boot-log-", ".tmp", context.getFilesDir());
            try {
                Files.deleteIfExists(file(context, true).toPath());
                if (!empty.renameTo(file(context, false))) {
                    throw new IOException("Could not clear the boot log");
                }
            } finally {
                Files.deleteIfExists(empty.toPath());
            }
        }
    }

    static String tail(Context context) {
        try (RandomAccessFile file = new RandomAccessFile(file(context, false), "r")) {
            long length = file.length();
            byte[] bytes = new byte[(int) Math.min(length, 8 * 1024)];
            file.seek(length - bytes.length);
            file.readFully(bytes);
            return new String(bytes, StandardCharsets.UTF_8);
        } catch (IOException ignored) {
            return "";
        }
    }
}
