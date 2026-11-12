package org.unixnu.eldensvc;

import android.content.Context;

import com.wireguard.android.backend.GoBackend;
import com.wireguard.android.backend.Tunnel;
import com.wireguard.config.Config;
import com.wireguard.config.InetNetwork;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

final class WireGuard implements Tunnel {
    private static final String SEALED_PROFILE = "wireguard.conf.sealed";
    private static final String PROFILE_KEY = "wireguard.key";
    private static final String LABEL = "elden-wireguard-v1";
    static final String ADDRESS = "10.8.0.8";
    private static WireGuard instance;

    private final Context context;
    private final GoBackend backend;
    private volatile LogServer server;
    private volatile State state = State.DOWN;
    private volatile String profileStatus = "Checking WireGuard profile";

    private WireGuard(Context context) {
        this.context = context.getApplicationContext();
        backend = new GoBackend(this.context);
    }

    static synchronized WireGuard get(Context context) {
        if (instance == null) instance = new WireGuard(context);
        return instance;
    }

    static File profile(Context context) {
        return new File(context.getFilesDir(), "wireguard.conf");
    }

    static boolean configured(Context context) {
        return profile(context).isFile() || new File(context.getFilesDir(), PROFILE_KEY).isFile();
    }

    private static byte[] readBounded(InputStream input, int limit) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int count;
        while ((count = input.read(buffer)) != -1) {
            if (bytes.size() + count > limit) throw new IOException("WireGuard profile is too large");
            bytes.write(buffer, 0, count);
        }
        return bytes.toByteArray();
    }

    private InputStream openProfile() throws Exception {
        File profile = profile(context);
        if (profile.isFile()) return new FileInputStream(profile);
        byte[] key;
        byte[] sealed;
        try (InputStream keyInput = new FileInputStream(new File(context.getFilesDir(), PROFILE_KEY));
             InputStream asset = context.getAssets().open(SEALED_PROFILE)) {
            key = readBounded(keyInput, 32);
            sealed = readBounded(asset, 64 * 1024);
        }
        try {
            if (key.length != 32 || sealed.length <= 28) throw new IOException("Invalid sealed profile");
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"),
                    new GCMParameterSpec(128, sealed, 0, 12));
            cipher.updateAAD(LABEL.getBytes(StandardCharsets.US_ASCII));
            return new ByteArrayInputStream(cipher.doFinal(sealed, 12, sealed.length - 12));
        } finally {
            Arrays.fill(key, (byte) 0);
        }
    }

    static Config lanConfig(InputStream input) throws Exception {
        Config original = Config.parse(input);
        if (original.getPeers().size() != 1 || !original.getInterface().getAddresses()
                .contains(InetNetwork.parse(ADDRESS + "/32"))) {
            throw new IOException("Expected the Elden Kindle peer");
        }
        // Keep the supplied identity and endpoint. Route the debug LAN through the tunnel.
        String lan = original.toWgQuickString()
                .replaceAll("(?m)^DNS[ \\t]*=.*\\n", "")
                .replaceAll("(?m)^AllowedIPs[ \\t]*=.*$", "AllowedIPs = 10.8.0.0/24")
                .replaceAll("(?m)^PersistentKeepalive[ \\t]*=.*\\n", "")
                + "PersistentKeepalive = 25\n";
        return Config.parse(new ByteArrayInputStream(lan.getBytes(StandardCharsets.UTF_8)));
    }

    void start() throws Exception {
        Config config;
        try (InputStream input = openProfile()) {
            config = lanConfig(input);
        } catch (Exception error) {
            throw new IOException("The WireGuard profile is unavailable or invalid");
        }
        backend.setState(this, State.UP, config);
    }

    void checkProfile() {
        if (!configured(context)) {
            profileStatus = "WireGuard profile missing";
            return;
        }
        try (InputStream input = openProfile()) {
            lanConfig(input);
            profileStatus = "WireGuard profile verified";
        } catch (Exception error) {
            profileStatus = "WireGuard profile unavailable or invalid";
        }
    }

    void stop() throws Exception {
        backend.setState(this, State.DOWN, null);
    }

    String status() {
        if (state == State.DOWN) return profileStatus;
        LogServer current = server;
        return current != null && current.isListening()
                ? "WireGuard on · http://" + ADDRESS + ":8734/latest"
                : "WireGuard on · starting LAN log listener";
    }

    @Override
    public String getName() {
        return "elden-lan";
    }

    @Override
    public synchronized void onStateChange(State newState) {
        state = newState;
        if (server != null) server.close();
        server = null;
        if (newState == State.UP) {
            server = new LogServer(context);
            new Thread(server, "elden-log-http").start();
        }
    }
}
