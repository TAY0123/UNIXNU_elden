package org.unixnu.eldensvc;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

final class LogServer implements Runnable, AutoCloseable {
    private final Context context;
    private volatile boolean running = true;
    private volatile ServerSocket listener;
    private volatile Socket client;

    LogServer(Context context) {
        this.context = context;
    }

    boolean isListening() {
        ServerSocket socket = listener;
        return socket != null && !socket.isClosed() && socket.isBound();
    }

    @Override
    public void run() {
        while (running) {
            try (ServerSocket socket = new ServerSocket()) {
                listener = socket;
                if (!running) return;
                socket.setReuseAddress(true);
                socket.bind(new InetSocketAddress(WireGuard.ADDRESS, 8734));
                while (running) {
                    try (Socket incoming = socket.accept()) {
                        client = incoming;
                        if (!running) return;
                        serve(incoming);
                    } catch (IOException ignored) {
                        if (socket.isClosed()) break;
                    } finally {
                        client = null;
                    }
                }
            } catch (IOException ignored) {
                if (running) android.os.SystemClock.sleep(1500);
            } finally {
                listener = null;
            }
        }
    }

    private static void headers(OutputStream out, String status, long length) throws IOException {
        String header = "HTTP/1.1 " + status + "\r\nContent-Type: text/plain; charset=utf-8\r\n"
                + "Content-Length: " + length + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        out.write(header.getBytes(StandardCharsets.US_ASCII));
    }

    private void serve(Socket socket) throws IOException {
        socket.setSoTimeout(3000);
        if (!"10.8.0.1".equals(socket.getInetAddress().getHostAddress())) return;
        InputStream input = socket.getInputStream();
        ByteArrayOutputStream request = new ByteArrayOutputStream();
        int last = 0;
        boolean complete = false;
        for (int i = 0; i < 8192; i++) {
            int next = input.read();
            if (next < 0) return;
            request.write(next);
            last = (last << 8) | next;
            if (last == 0x0d0a0d0a || (last & 0xffff) == 0x0a0a) {
                complete = true;
                break;
            }
        }
        if (!complete) return;
        String[] line = request.toString(StandardCharsets.US_ASCII.name()).split("\\r?\\n", 2);
        String[] words = line[0].split(" ", 3);
        OutputStream output = socket.getOutputStream();
        if (words.length != 3) {
            headers(output, "400 Bad Request", 0);
            return;
        }
        boolean head = "HEAD".equals(words[0]);
        if (!head && !"GET".equals(words[0])) {
            headers(output, "405 Method Not Allowed", 0);
            return;
        }
        boolean previous = "/previous".equals(words[1]);
        File file = BootLog.file(context, previous);
        if ((!previous && !"/latest".equals(words[1])) || !file.isFile()) {
            headers(output, "404 Not Found", 0);
            return;
        }
        try (FileInputStream data = new FileInputStream(file)) {
            long remaining = data.getChannel().size();
            headers(output, "200 OK", remaining);
            if (head) return;
            byte[] bytes = new byte[8192];
            while (remaining > 0) {
                int count = data.read(bytes, 0, (int) Math.min(bytes.length, remaining));
                if (count < 0) break;
                output.write(bytes, 0, count);
                remaining -= count;
            }
        }
    }

    @Override
    public void close() {
        running = false;
        try {
            Socket incoming = client;
            if (incoming != null) incoming.close();
        } catch (IOException ignored) {
        }
        try {
            ServerSocket socket = listener;
            if (socket != null) socket.close();
        } catch (IOException ignored) {
        }
    }
}
