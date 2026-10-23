"""Type a text file (or a string) on the PC through the Android HID keyboard (hidkbd).

usage:
  python hidtype.py FILE [options]     type the contents of FILE
  python hidtype.py -t "text" [options]
  python hidtype.py --install          copy hidkbd and scripts to the phone (after build.sh)
  python hidtype.py --start            switch the phone's USB to HID mode
  python hidtype.py --stop             restore the original USB mode

abort while typing: press Esc (anywhere on the PC), Ctrl+C here, or volume-down on the phone.

options:
  -s SERIAL          adb device serial (default: $ANDROID_SERIAL or the only device)
  -w SEC             wait SEC seconds before typing (time to focus the target window)
  --layout jp|us     keyboard layout of the target PC (default jp)
  --case MODE        normal | invert (swap Shift for letters) | auto (invert while CapsLock LED is on)
  --delay MS         extra delay per key
  --hold MS          keep each key state (press / release) for at least MS. Use ~20-35 for game consoles
                     and other hosts that read the keyboard once per frame (16.7 ms); 0 = fastest
  --autoindent       target editor copies the previous line's indent on Enter:
                     remove/add only the difference with Backspace
  --from-line N      resume from line N. With --autoindent the cursor must be at the END of line N-1
                     (a newline is typed first); otherwise at the start of an empty line N.
  --show             print the key stream (Backspace shown as <BS>) and exit
"""
import argparse
import bisect
import os
import subprocess
import sys
import tempfile
import time

REMOTE = "/data/local/tmp/hidkbd"
TYPABLE_CTRL = b"\t\n"


def adb_cmd(serial):
    return ["adb"] + (["-s", serial] if serial else [])


def leading_ws(line):
    i = 0
    while i < len(line) and line[i] in " \t":
        i += 1
    return line[:i]


def build_stream(text, autoindent, from_line):
    """Returns (stream, line_starts): line_starts[k] = stream offset where source line (from_line+k) begins."""
    lines = text.split("\n")
    if from_line > len(lines):
        sys.exit(f"--from-line {from_line} is past the end ({len(lines)} lines)")
    out, starts = [], []
    pos = 0
    prev_ws = leading_ws(lines[from_line - 2]) if from_line > 1 else ""
    for idx in range(from_line - 1, len(lines)):
        line = lines[idx]
        if idx > from_line - 1 or (from_line > 1 and autoindent):
            out.append("\n")
            pos += 1
        starts.append(pos)
        if autoindent:
            cur_ws = leading_ws(line)
            common = 0
            while common < min(len(prev_ws), len(cur_ws)) and prev_ws[common] == cur_ws[common]:
                common += 1
            seg = "\b" * (len(prev_ws) - common) + line[common:]
            prev_ws = cur_ws
        else:
            seg = line
        out.append(seg)
        pos += len(seg)
    return "".join(out), starts


class Shell:
    """Persistent root shell on the phone (one adb round-trip per command)."""

    def __init__(self, serial):
        self.p = subprocess.Popen(adb_cmd(serial) + ["shell", "su"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)

    def run(self, cmd):
        self.p.stdin.write(cmd + "; echo __END__\n")
        self.p.stdin.flush()
        lines = []
        while True:
            l = self.p.stdout.readline()
            if not l:
                raise RuntimeError("adb shell closed")
            l = l.rstrip("\r\n")
            if l == "__END__":
                return lines
            lines.append(l)

    def close(self):
        try:
            self.p.stdin.write("exit\n")
            self.p.stdin.flush()
            self.p.wait(3)
        except Exception:
            self.p.kill()


def esc_pressed_factory():
    if os.name != "nt":
        return lambda: False
    import ctypes
    gaks = ctypes.windll.user32.GetAsyncKeyState
    gaks(0x1B)  # clear the "pressed since last call" bit
    return lambda: bool(gaks(0x1B) & 0x8001)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("file", nargs="?")
    p.add_argument("-t", "--text")
    p.add_argument("-s", "--serial", default=os.environ.get("ANDROID_SERIAL"))
    p.add_argument("-w", "--wait", type=float, default=0)
    p.add_argument("--install", action="store_true")
    p.add_argument("--start", action="store_true")
    p.add_argument("--stop", action="store_true")
    p.add_argument("--layout", default="jp", choices=["jp", "us"])
    p.add_argument("--case", default="normal", choices=["normal", "invert", "auto"])
    p.add_argument("--delay", type=int, default=0)
    p.add_argument("--hold", type=int, default=0)
    p.add_argument("--autoindent", action="store_true")
    p.add_argument("--from-line", type=int, default=1)
    p.add_argument("--show", action="store_true")
    a = p.parse_args()

    if a.install:
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
        files = [os.path.join(root, f) for f in ("out/hidkbd", "device/hid_start.sh", "device/hid_stop.sh")]
        subprocess.run(adb_cmd(a.serial) + ["shell", f"mkdir -p {REMOTE}"], check=True)
        subprocess.run(adb_cmd(a.serial) + ["push"] + files + [REMOTE + "/"], check=True)
        subprocess.run(adb_cmd(a.serial) + ["shell", f"chmod 755 {REMOTE}/hidkbd"], check=True)
        return
    if a.start or a.stop:
        script = "hid_start.sh" if a.start else "hid_stop.sh"
        subprocess.run(adb_cmd(a.serial) + ["shell", f"su -c 'sh {REMOTE}/{script}'"], check=True)
        return

    if a.text is not None:
        raw = a.text.encode("utf-8")
    elif a.file:
        with open(a.file, "rb") as f:
            raw = f.read()
    else:
        p.error("FILE or -t TEXT is required")
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    text = raw.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")

    bad = {}
    for n, line in enumerate(text.split("\n"), 1):
        for ch in line:
            if ord(ch) >= 0x80 or (ord(ch) < 0x20 and ch != "\t"):
                bad.setdefault(n, ch)
    if bad:
        first = ", ".join(f"line {n} ({c!r})" for n, c in list(bad.items())[:5])
        print(f"warning: {len(bad)} line(s) contain characters that will be skipped: {first}"
              f"{' ...' if len(bad) > 5 else ''}", file=sys.stderr)

    stream, starts = build_stream(text, a.autoindent, a.from_line)
    if a.show:
        sys.stdout.write(stream.replace("\b", "<BS>"))
        sys.stdout.write("\n")
        return
    data = stream.encode("utf-8")
    # byte offsets for line lookup (stream may contain multibyte chars that get skipped)
    byte_starts = [len(stream[:s].encode("utf-8")) for s in starts] if not stream.isascii() else starts

    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as tf:
        tf.write(data)
    try:
        subprocess.run(adb_cmd(a.serial) + ["push", tf.name, f"{REMOTE}/in.txt"], check=True,
                       stdout=subprocess.DEVNULL)
    finally:
        os.unlink(tf.name)

    sh = Shell(a.serial)
    if sh.run(f"grep -q hidkbd /proc/$(cat {REMOTE}/pid)/cmdline 2>/dev/null && echo ok") != ["ok"]:
        sys.exit("hidkbd is not running: run  python hidtype.py --start")

    esc = esc_pressed_factory()
    try:
        if a.wait:
            end = time.time() + a.wait
            while time.time() < end:
                print(f"\rtyping in {end - time.time():4.1f}s ... focus the target window (Esc = cancel)",
                      end="", flush=True)
                if esc():
                    print("\ncancelled")
                    return
                time.sleep(0.05)
            print()
        sh.run(f"echo 'queued 0 {len(data)}' > {REMOTE}/status")
        sh.run(f"echo 'type {a.layout} {a.case} {a.delay} {a.hold} {REMOTE}/in.txt' > {REMOTE}/ctl")
        t0 = time.time()
        state, pos, total = "queued", 0, len(data)
        aborted_by = None
        while True:
            if esc():
                aborted_by = "Esc"
            if aborted_by:
                sh.run(f"echo abort > {REMOTE}/ctl")
                time.sleep(0.2)
            st = sh.run(f"cat {REMOTE}/status")
            if st:
                f = st[0].split()
                if len(f) == 3:
                    state, pos, total = f[0], int(f[1]), int(f[2])
            line = a.from_line + max(0, bisect.bisect_right(byte_starts, pos) - 1)
            line = min(line, a.from_line + len(byte_starts) - 1)
            rate = pos / max(time.time() - t0, 1e-3)
            print(f"\r{state:8s} {pos}/{total} bytes  line {line}  {rate:6.0f} B/s ", end="", flush=True)
            if state in ("done", "aborted", "error"):
                break
            time.sleep(0.1)
        print()
        if state == "aborted":
            print(f"aborted{' by ' + aborted_by if aborted_by else ''} at line {line}.")
            if a.autoindent:
                print(f"to resume: delete line {line}'s partial text so the cursor is at the end of line {line - 1}, "
                      f"then run with --from-line {line}")
            else:
                print(f"to resume: clear line {line} and run with --from-line {line}")
        elif state == "error":
            print("error: see  adb shell su -c cat /data/local/tmp/hidkbd/log")
    except KeyboardInterrupt:
        sh.run(f"echo abort > {REMOTE}/ctl")
        print("\naborted by Ctrl+C")
    finally:
        sh.close()


if __name__ == "__main__":
    main()
