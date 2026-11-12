package org.unixnu.eldensvc;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;
import java.util.function.Consumer;

final class Fastboot {
    static final long BACKUP_SIZE = 96L * 1024 * 1024;
    static final String BACKUP_HASH = "29a3171cffc97b4e7c073bbeb00fc7cb48223bc3611594224b723fbdf51e7070";
    private static final int COMMAND_TIMEOUT = 12_000;

    interface Transport {
        void send(byte[] bytes, int length, int timeout) throws IOException;
        int receive(byte[] bytes, int timeout) throws IOException;
    }

    private final Transport transport;
    private final Consumer<String> progress;
    private final byte[] reply = new byte[256];

    Fastboot(Transport transport, Consumer<String> progress) {
        this.transport = transport;
        this.progress = progress;
    }

    private String response(String expected, int timeout) throws IOException {
        long deadline = System.nanoTime() + timeout * 1_000_000L;
        for (;;) {
            long remaining = (deadline - System.nanoTime()) / 1_000_000;
            if (remaining <= 0) throw new IOException("Fastboot response timed out");
            int count = transport.receive(reply, (int) remaining);
            if (count < 4) throw new IOException("Incomplete fastboot response");
            String type = new String(reply, 0, 4, StandardCharsets.US_ASCII);
            String detail = new String(reply, 4, count - 4, StandardCharsets.UTF_8).trim();
            switch (type) {
                case "INFO", "TEXT" -> progress.accept(detail);
                case "FAIL" -> throw new IOException("Bootloader refused the command. " + detail);
                default -> {
                    if (!expected.equals(type)) throw new IOException("Unexpected fastboot response " + type);
                    return detail;
                }
            }
        }
    }

    private String command(String command, String expected, int timeout) throws IOException {
        byte[] bytes = command.getBytes(StandardCharsets.US_ASCII);
        transport.send(bytes, bytes.length, COMMAND_TIMEOUT);
        return response(expected, timeout);
    }

    private String variable(String name) throws IOException {
        return command("getvar:" + name, "OKAY", COMMAND_TIMEOUT);
    }

    void verifyTablet() throws IOException {
        if (!"elden_prc_wifi".equals(variable("product"))) throw new IOException("Unexpected tablet product");
        if (!"no".equals(variable("is-userspace"))) throw new IOException("Use bootloader fastboot on the tablet");
    }

    String slot() throws IOException {
        String slot = variable("current-slot");
        if (!"a".equals(slot) && !"b".equals(slot)) throw new IOException("Unknown tablet slot");
        return slot;
    }

    void activate(String slot) throws IOException {
        if (!"a".equals(slot) && !"b".equals(slot)) throw new IllegalArgumentException("Invalid slot");
        command("set_active:" + slot, "OKAY", 30_000);
        if (!slot.equals(slot())) throw new IOException("The bootloader reported a different active slot");
    }

    void reboot() throws IOException {
        command("reboot", "OKAY", COMMAND_TIMEOUT);
    }

    static void verifyBackup(File image) throws IOException {
        if (image.length() != BACKUP_SIZE) throw new IOException("The Linux backup must be 96 MiB");
        try (FileInputStream input = new FileInputStream(image)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] bytes = new byte[64 * 1024];
            int count;
            while ((count = input.read(bytes)) != -1) digest.update(bytes, 0, count);
            StringBuilder hex = new StringBuilder();
            for (byte value : digest.digest()) hex.append(String.format(Locale.ROOT, "%02x", value & 255));
            if (!BACKUP_HASH.contentEquals(hex)) throw new IOException("The Linux backup checksum differs");
        } catch (NoSuchAlgorithmException error) {
            throw new IOException("SHA-256 is unavailable", error);
        }
    }

    void restore(File image) throws IOException {
        verifyBackup(image);
        String partition = variable("partition-size:boot_b");
        if (!partition.matches("0[xX][0-9a-fA-F]{1,16}")
                || Long.parseUnsignedLong(partition.substring(2), 16) != BACKUP_SIZE
                || image.length() != BACKUP_SIZE) {
            throw new IOException("The image and boot_b must both be 96 MiB");
        }
        String size = String.format(Locale.ROOT, "%08x", BACKUP_SIZE);
        if (!size.equalsIgnoreCase(command("download:" + size, "DATA", COMMAND_TIMEOUT))) {
            throw new IOException("The tablet requested a different download size");
        }
        long sent = 0;
        byte[] buffer = new byte[16 * 1024];
        try (FileInputStream input = new FileInputStream(image)) {
            while (sent < BACKUP_SIZE) {
                int read = input.read(buffer);
                if (read <= 0) throw new IOException("The backup ended during upload");
                transport.send(buffer, read, COMMAND_TIMEOUT);
                sent += read;
                if (sent % (4 * 1024 * 1024) == 0) {
                    progress.accept("Sent " + (sent / (1024 * 1024)) + " of 96 MiB");
                }
            }
        }
        response("OKAY", 30_000);
        progress.accept("Writing boot_b");
        command("flash:boot_b", "OKAY", 180_000);
    }
}
