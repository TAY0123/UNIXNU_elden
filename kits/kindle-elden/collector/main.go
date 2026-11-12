package main

import (
	"bytes"
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"
)

const logLimit = 16 << 20

type bootLog struct {
	dir  string
	file *os.File
	size int64
}

func (l *bootLog) open() error {
	file, err := os.OpenFile(filepath.Join(l.dir, "latest.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	info, err := file.Stat()
	if err != nil {
		file.Close()
		return err
	}
	l.file, l.size = file, info.Size()
	return nil
}

func (l *bootLog) Write(p []byte) (int, error) {
	if l.size+int64(len(p)) > logLimit {
		if err := os.Rename(filepath.Join(l.dir, "latest.log"), filepath.Join(l.dir, "previous.log")); err != nil {
			return 0, err
		}
		l.file.Close()
		if err := l.open(); err != nil {
			return 0, err
		}
	}
	n, err := l.file.Write(p)
	l.size += int64(n)
	return n, err
}

func wait(ctx context.Context, d time.Duration) {
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-timer.C:
	case <-ctx.Done():
	}
}

func serve(ctx context.Context, dir, address, reader string) {
	mux := http.NewServeMux()
	for _, name := range []string{"latest", "previous"} {
		mux.HandleFunc("/"+name, func(w http.ResponseWriter, r *http.Request) {
			host, _, err := net.SplitHostPort(r.RemoteAddr)
			if err != nil || host != reader {
				http.Error(w, "Forbidden", http.StatusForbidden)
				return
			}
			if r.Method != http.MethodGet && r.Method != http.MethodHead {
				w.WriteHeader(http.StatusMethodNotAllowed)
				return
			}
			w.Header().Set("Content-Type", "text/plain; charset=utf-8")
			w.Header().Set("Cache-Control", "no-store")
			http.ServeFile(w, r, filepath.Join(dir, name+".log"))
		})
	}
	for ctx.Err() == nil {
		listener, err := net.Listen("tcp4", address)
		if err != nil {
			wait(ctx, 2*time.Second)
			continue
		}
		server := &http.Server{Handler: mux, ReadHeaderTimeout: 5 * time.Second, WriteTimeout: 30 * time.Second}
		stop := context.AfterFunc(ctx, func() { server.Close() })
		err = server.Serve(listener)
		stop()
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("Log server: %v", err)
			wait(ctx, 2*time.Second)
		}
	}
}

func main() {
	dir := flag.String("directory", "/var/local/elden-tools/logs", "private log directory")
	address := flag.String("listen", "10.8.0.8:8734", "WireGuard listen address")
	reader := flag.String("reader", "10.8.0.1", "WireGuard peer allowed to read logs")
	usb := flag.String("usb-reader", "", "Elden USB CDC reader executable")
	flag.Parse()
	if *usb == "" {
		log.Fatal("Specify the USB reader executable")
	}
	if err := os.MkdirAll(*dir, 0700); err != nil {
		log.Fatal(err)
	}
	capture := bootLog{dir: *dir}
	if err := capture.open(); err != nil {
		log.Fatal(err)
	}
	defer func() { capture.file.Close() }()
	if _, err := fmt.Fprintf(&capture, "\n[%s] Waiting for Elden USB CDC\n", time.Now().UTC().Format(time.RFC3339)); err != nil {
		log.Fatal(err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	go serve(ctx, *dir, *address, *reader)
	lastDiagnostic := ""
	for ctx.Err() == nil {
		command := exec.CommandContext(ctx, *usb)
		var stderr bytes.Buffer
		command.Stdout = &capture
		command.Stderr = &stderr
		err := command.Run()
		var exit *exec.ExitError
		if err != nil && ctx.Err() == nil && (!errors.As(err, &exit) || exit.ExitCode() != 2) {
			message := fmt.Sprintf("USB reader: %v %s", err, stderr.String())
			if message != lastDiagnostic {
				log.Print(message)
				lastDiagnostic = message
			}
		} else {
			lastDiagnostic = ""
		}
		wait(ctx, time.Second)
	}
}
