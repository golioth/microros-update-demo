#!/usr/bin/env python3
"""Persistent Tikk console holder: one fd, one DTR, forever.

- Resolves the Tikk console CDC by ID_SERIAL_SHORT + usb-if 00 on every
  (re)open (ttyACM minors reshuffle on board resets).
- Holds DTR the entire time — cycling DTR (open/close probes) wedges the
  4.4 legacy CDC RX path, so NOTHING else may open this port while this
  daemon runs.
- Commands: write a line to /tmp/tikk-cmd.fifo (e.g. `fs ls /lfs1`).
- All console output lands in /tmp/tikk-console-live.log (append).

Log hygiene (the Zephyr shell is VT100-chatty: bold-green prompts,
cursor-left + erase-to-end-of-screen redraws, CRs, and it re-emits every
log line a second time prefixed by its prompt — rendered by `tail -f`
that looks like the log "overwrites itself"):
- ALL ANSI/VT100 CSI sequences and CRs are stripped before logging, so
  tail -f renders stable, non-destructive lines.
- The shell's immediate prompt-prefixed redraw copies of a line (and
  bare prompts) are dropped, halving the noise. Real repeats are safe:
  log lines carry timestamps, so only the byte-identical immediate
  copy is suppressed (window: last 5 lines).
- OTA-related lines are wrapped in ANSI escapes (black on yellow,
  \\x1b[30;43m) so `tail -f` highlights the OTA arc — the manifest/
  version line, download progress, and the swap/reboot. Stripping
  first means nothing can interrupt the highlight mid-line.
"""
import collections
import os
import re
import select
import subprocess
import termios
import fcntl
import array
import time

TIKK_SERIAL = "5F5F7992AC89B3D0"
LOG = "/tmp/tikk-console-live.log"
FIFO = "/tmp/tikk-cmd.fifo"

# substrings that mark an OTA-relevant line (matched on the CLEANED line)
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

ANSI_CSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
PROMPT = b"uart:~$ "

# byte-identical immediate shell-redraw copies land within a few lines;
# timestamps make false positives on real events effectively impossible
recent = collections.deque(maxlen=5)

if not os.path.exists(FIFO):
    os.mkfifo(FIFO)


def clean_line(raw):
    """Strip VT100/ANSI + CRs, drop prompt prefixes and redraw copies.

    Returns the cleaned line bytes, or None if the line should not be
    logged (bare prompt, or a duplicate of a just-seen line).
    """
    line = ANSI_CSI.sub(b"", raw).replace(b"\r", b"")
    while line.startswith(PROMPT):
        line = line[len(PROMPT):]
        line = line.lstrip() if not line.startswith(PROMPT) else line
    if not line.strip():
        return None
    if line in recent:
        return None
    recent.append(line)
    return line


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
print(f"console daemon: port={port} (VT100-stripped, dedup, OTA highlight)", flush=True)

line_buf = b""          # pending bytes of an incomplete line
last_data = time.time()

with open(LOG, "ab", buffering=0) as out:
    def emit(raw_line):
        cleaned = clean_line(raw_line)
        if cleaned is None:
            return
        if any(m.encode() in cleaned for m in OTA_MARKERS):
            out.write(ANSI_HL + cleaned + ANSI_RESET + b"\n")
        else:
            out.write(cleaned + b"\n")

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
                    emit(line)
            elif line_buf and (time.time() - last_data) > 2.0:
                # tail without newline (rare after VT100 stripping) that
                # has been idle: show it unprocessed
                emit(line_buf)
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