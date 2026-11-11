#!/usr/bin/env python3
"""Control Elden's UniXNU USB menu and capture firmware and Recovery logs on macOS."""

import argparse
import fcntl
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import sys
import termios
import time
import tty


def serial_port():
    ports = []
    for path in Path('/dev').glob('cu.usbmodem*'):
        suffix = path.name.removeprefix('cu.usbmodem')
        match = re.fullmatch(r'ELDEN[-_]?(UEFI|XNU)[0-9]*', suffix, re.I)
        if match:
            ports.append((match[1].upper(), path))
    if len(ports) > 1:
        raise RuntimeError('Multiple Elden management ports are present')
    return ports[0] if ports else (None, None)


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
    try:
        tty.setraw(fd, termios.TCSANOW)
        settings = termios.tcgetattr(fd)
        settings[4] = settings[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack('i', termios.TIOCM_DTR))
    except BaseException:
        os.close(fd)
        raise
    return fd


def send(fd, command):
    data = (command + '\n').encode('ascii')
    while data:
        if not select.select([], [fd], [], 2)[1]:
            raise TimeoutError('Elden serial command write timed out')
        data = data[os.write(fd, data):]


def command(*args, timeout=6):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def fastboot_device():
    result = command('fastboot', 'devices')
    serials = [line.split()[0] for line in result.stdout.splitlines()
               if len(line.split()) == 2 and line.split()[1] == 'fastboot']
    elden = []
    for serial in serials:
        product = command('fastboot', '-s', serial, 'getvar', 'product')
        if product.returncode or not re.search(r'\bproduct:\s*elden(?:_prc_wifi)?\s*$',
                                               product.stdout + product.stderr, re.M):
            continue
        mode = command('fastboot', '-s', serial, 'getvar', 'is-userspace')
        if mode.returncode == 0 and re.search(r'\bis-userspace:\s*no\s*$',
                                               mode.stdout + mode.stderr, re.M):
            elden.append(serial)
    if len(elden) > 1:
        raise RuntimeError('Multiple Elden fastboot devices are present')
    return elden[0] if elden else None


def linux_to_fastboot():
    if command('adb', '-s', 'TB322FC', 'get-state').stdout.strip() != 'device':
        return False
    if 'y700' not in command('adb', '-s', 'TB322FC', 'shell', 'uname', '-r').stdout:
        return False
    result = command('adb', '-s', 'TB322FC', 'shell', 'sudo', '-n', 'systemctl',
                     'reboot', '--reboot-argument=bootloader')
    if result.returncode:
        raise RuntimeError(f'Linux reboot command failed: {result.stderr.strip()}')
    return True


def run(args, log):
    def record(data):
        log.write(data)
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

    deadline = time.monotonic() + args.seconds
    fd, previous = None, None
    sent = False
    requested_fastboot = False
    ram_booted = False
    received = 0
    received_xnu = False
    boot_acknowledged = False
    panic_captured = False
    recovery_boot = args.operation == 'boot' and args.entry.startswith('recovery')
    last_fastboot_check = 0.0
    latest = b''
    try:
        while time.monotonic() < deadline:
            mode, path = serial_port()
            if mode == 'XNU' and args.no_xnu_console:
                path = None
            if path != previous:
                if fd is not None:
                    os.close(fd)
                    fd = None
                previous = None
                if path:
                    try:
                        fd = open_port(path)
                    except OSError:
                        time.sleep(0.15)
                        continue
                    previous = path
                    record(f'\n[USB {mode} connected]\n'.encode())
            if not path:
                if args.operation == 'restart' and sent:
                    record(b'\n[Elden USB disconnected for restart]\n')
                    return 0
                now = time.monotonic()
                watch_fastboot = args.operation == 'fastboot' or args.ram_image or (recovery_boot and sent)
                if watch_fastboot and now - last_fastboot_check > 1:
                    last_fastboot_check = now
                    device = fastboot_device()
                    if device and args.operation == 'fastboot':
                        record(b'\n[Elden entered bootloader fastboot]\n')
                        return 0
                    if device and recovery_boot and sent:
                        record(b'\n[Recovery returned to bootloader fastboot]\n')
                        if args.require_fastboot and not (boot_acknowledged or received_xnu):
                            record(b'[Recovery handoff was not acknowledged]\n')
                            return 1
                        return 0 if args.require_fastboot or received_xnu else 1
                    if device and args.ram_image and not ram_booted:
                        result = command('fastboot', '-s', device, 'boot', str(args.ram_image), timeout=90)
                        record((result.stdout + result.stderr).encode())
                        if result.returncode:
                            return result.returncode
                        ram_booted = True
                    elif (args.operation == 'fastboot' or (args.ram_image and not ram_booted)) and not requested_fastboot:
                        requested_fastboot = linux_to_fastboot()
                        if requested_fastboot:
                            record(b'\n[Linux accepted the USB request to enter bootloader fastboot]\n')
                time.sleep(0.15)
                continue
            if args.ram_image and not ram_booted:
                if not requested_fastboot:
                    request = 'reboot-fastboot' if mode == 'XNU' else 'boot fastboot'
                    send(fd, request)
                    record(f'[host sent {request}]\n'.encode())
                    requested_fastboot = True
            elif not sent:
                request = None
                if mode == 'UEFI':
                    if args.operation == 'boot':
                        request = 'boot ' + args.entry
                    elif args.operation == 'list':
                        request = 'list'
                    elif args.operation == 'slot':
                        request = 'boot-confirmed slot-' + args.entry
                    elif args.operation in ('restart', 'fastboot'):
                        request = 'boot ' + args.operation
                elif args.operation in ('restart', 'fastboot'):
                    request = 'reboot' if args.operation == 'restart' else 'reboot-fastboot'
                if request:
                    send(fd, request)
                    record(f'[host sent {request}]\n'.encode())
                    sent = True
            if not select.select([fd], [], [], 0.2)[0]:
                continue
            try:
                data = os.read(fd, 16384)
            except BlockingIOError:
                continue
            except OSError:
                data = b''
            if not data:
                os.close(fd)
                fd, previous = None, None
                time.sleep(0.15)
                continue
            received += len(data)
            received_xnu = received_xnu or mode == 'XNU'
            record(data)
            latest = (latest + data)[-4096:]
            if sent and args.operation == 'boot':
                boot_acknowledged = boot_acknowledged or (
                    f'tty: booting {args.entry}\r\n'.encode() in latest)
            if sent and args.operation == 'slot' and re.search(rb'ELDEN SLOT: .*result=[-0-9]+[^\n]*\n', latest):
                return 0 if re.search(rb'ELDEN SLOT: .*result=0\b', latest) else 1
            if sent and args.operation == 'boot' and args.entry == 'slot-status' and b'ELDEN SLOT:' in latest:
                if re.search(rb'ELDEN SLOT: .*result=[-0-9]+[^\n]*\n', latest):
                    return 0 if re.search(rb'ELDEN SLOT: .*result=0\b', latest) else 1
            if sent and args.operation == 'list' and re.search(rb'tty: entry [0-9]+ id=', latest):
                deadline = min(deadline, time.monotonic() + 0.8)
            if recovery_boot and not panic_captured and b'ELDEN PANIC END' in latest:
                panic_captured = True
                record(b'\n[Complete Recovery panic captured]\n')
                if not args.require_fastboot:
                    deadline = time.monotonic() + 15
        if args.operation == 'capture':
            return 0 if received else 1
        if args.operation == 'list' and b'tty: entry' in latest:
            return 0
        if args.require_fastboot or panic_captured:
            record(b'\n[Recovery fastboot reset was not observed]\n')
            return 1
        if recovery_boot and sent and received_xnu:
            return 0
        if args.operation == 'boot' and args.entry != 'slot-status' and not args.entry.startswith('recovery') and boot_acknowledged:
            return 0
        raise TimeoutError('Requested Elden USB phase or result was not observed')
    finally:
        if fd is not None:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('capture', 'list', 'boot', 'slot', 'restart', 'fastboot'))
    parser.add_argument('entry', nargs='?', help='menu entry ID for boot, or a/b for slot')
    parser.add_argument('--ram-image', type=Path, help='temporarily boot this image before selecting the entry')
    parser.add_argument('--require-fastboot', action='store_true',
                        help='succeed only when Recovery returns to bootloader fastboot')
    parser.add_argument('--no-xnu-console', action='store_true',
                        help='leave the XNU serial port closed to test unattended reset')
    parser.add_argument('--seconds', type=float, default=120, help='maximum time to capture or wait')
    parser.add_argument('--log', type=Path, required=True, help='append the unmodified USB output to this file')
    args = parser.parse_args()
    if args.operation == 'boot' and not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,31}', args.entry or ''):
        parser.error('boot requires a menu entry ID')
    if args.operation == 'slot' and args.entry not in ('a', 'b'):
        parser.error('slot requires a or b')
    if args.operation not in ('boot', 'slot') and args.entry is not None:
        parser.error('this operation takes no entry ID')
    if args.ram_image and (args.operation != 'boot' or not args.ram_image.is_file()):
        parser.error('--ram-image requires boot and an existing file')
    if args.require_fastboot and (args.operation != 'boot' or not args.entry.startswith('recovery')):
        parser.error('--require-fastboot requires a Recovery boot entry')
    if args.no_xnu_console and not args.require_fastboot:
        parser.error('--no-xnu-console requires --require-fastboot')
    if not 1 <= args.seconds <= 3600:
        parser.error('--seconds must be between 1 and 3600')
    os.umask(0o077)
    try:
        with args.log.open('ab', buffering=0) as log:
            return run(args, log)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f'Elden USB: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
