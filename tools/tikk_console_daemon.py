#!/usr/bin/env python3
"""Persistent Tikk console holder: one fd, one DTR, forever.

- Resolves the Tikk console CDC by ID_SERIAL_SHORT + usb-if 00 on every
  (re)open (ttyACM minors reshuffle on board resets).
- Holds DTR the entire time — cycling DTR (open/close probes) wedges the
  4.4 legacy CDC RX path, so NOTHING else may open this port while this
  daemon runs.
- Commands: write a line to /tmp/tikk-cmd.fifo (e.g. `fs ls /lfs1`).
- All console output lands in /tmp/tikk-console-live.log (append).
- OTA-related lines are written wrapped in ANSI escapes (black on
  yellow, \\x1b[30;43m) so `tail -f` highlights the OTA arc — the
  manifest/version line, download progress, and the swap/reboot.
"""
import os, termios, fcntl, array, select, subprocess, time

TIKK_SERIAL = "5F5F7992AC89B3D0"
LOG = "/tmp/tikk-console-live.log"
FIFO = "/tmp/tikk-cmd.fifo"

# substrings that mark an OTA-relevant line (logged anywhere in the line —
# the shell prompt often prefixes log lines, so prefix matching is useless)
OTA_MARKERS = (
    "OTA",            # "OTA manifest: main@X ... (running Y)" / "OTA progress: N%"
    "fw_update",      # the fw_update module's log prefix
    ".u/desired",     # manifest downlink path
    ".u/c/",          # component data downlink path (.u/c/main@version)
    "Firmware download",
    "Image written",
    "Marking",        # "Marking main for download"
)
ANSI_HL = b"\x1b[30;43m"   # black on yellow
ANSI_RESET = b"\x1b[0m"

if not os.path.exists(FIFO):
    os.mkfifo(FIFO)


def find_port():
    try:
        out = subprocess.run(["ls", "/dev/"], capture_output=True, text=True).stdout
    except Exception:
        return None
    for name in sorted(l for l in out.splitlines() if l.startswith("ttyACM")):
        path = f"/dev/{name}"
        try:
            props = subprocess.run(["udevadm", "info", "-q", "property", "-n", path],
                                   capture_output=True, text=True).stdout
        except Exception:
            continue
        if f"ID_SERIAL_SHORT={TIKK_SERIAL}" in props and "ID_USB_INTERFACE_NUM=00" in props:
            return path
    return None


def open_port():
    path = find_port()
    if not path:
        return None, None
    try:
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError:
        return None, None
    for line in (termios.TIOCM_DTR, termios.TIOCM_RTS):
        fcntl.ioctl(fd, termios.TIOCMBIS, array.array("i", [line]))
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0; attrs[1] = 0; attrs[3] = 0
    attrs[6][termios.VTIME] = 0; attrs[6][termios.VMIN] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd, path


fifo_fd = os.open(FIFO, os.O_RDONLY | os.O_NONBLOCK)
fd, port = open_port()
print(f"console daemon: port={port} (OTA lines highlighted)", flush=True)

line_buf = b""          # pending bytes of an incomplete line
last_data = time.time()

with open(LOG, "ab", buffering=0) as out:
    def write_line(raw, complete):
        if complete and any(m.encode() in raw for m in OTA_MARKERS):
            out.write(ANSI_HL + raw + ANSI_RESET + b"\n")
        else:
            out.write(raw)

    while True:
        try:
            if fd is None:
                time.sleep(1.0)
                fd, port = open_port()
                if fd:
                    print(f"console daemon: reopened {port}", flush=True)
                continue
            r, _, _ = select.select([fd, fifo_fd], [], [], 0.5)
            if fifo_fd in r:
                cmd = os.read(fifo_fd, 512)
                if cmd:
                    try:
                        os.write(fd, cmd if cmd.endswith(b"\n") else cmd + b"\n")
                    except OSError:
                        fd = None
            if fd in r:
                try:
                    chunk = os.read(fd, 4096)
                except OSError:
                    fd = None
                    continue
                if not chunk:
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                    fd = None
                    continue
                last_data = time.time()
                line_buf += chunk
                while b"\n" in line_buf:
                    line, line_buf = line_buf.split(b"\n", 1)
                    write_line(line, complete=True)
            elif line_buf and (time.time() - last_data) > 2.0:
                # tail without newline (e.g. the bare shell prompt) that
                # has been idle: show it unhighlighted
                write_line(line_buf, complete=False)
                line_buf = b""
        except Exception as e:
            # never die: log, drop the fd, re-resolve next loop
            print(f"console daemon: recovered from {e!r}", flush=True)
            try:
                os.close(fd)
            except Exception:
                pass
            fd = None
            time.sleep(1.0)