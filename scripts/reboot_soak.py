#!/usr/bin/env python3
"""Reboot soak test for a SpiderBridge on a USB serial port.

A bridge must always come back on its own. This restarts it again and again and checks, every time:

  * it answers over USB again (Improv STATE), and how long that took,
  * the restart was a clean one (reset reason, crash counter, safe mode),
  * nothing accumulates (heap, restart-streak).

Three modes:

  command   the SpiderBridge restart command (Improv 0x44) -- what the installer and the web page do
  hard      pulse the EN pin through the serial control lines, like pressing the reset button
  fault     make the bridge FAIL on purpose (crash, task watchdog, interrupt watchdog, a stuck task, a
            memory leak) and check that it recovers by itself, and that three failures in a row end in
            SAFE MODE, which a restart then leaves. Uses the self-test command (0x46) over USB.

    python scripts/reboot_soak.py COM3 --cycles 20
    python scripts/reboot_soak.py /dev/ttyUSB0 --cycles 10 --mode hard
    python scripts/reboot_soak.py COM3 --mode fault                 # all five faults, then safe mode
    python scripts/reboot_soak.py COM3 --mode fault --fault panic    # one fault

Needs pyserial (python -m pip install pyserial). Exit code 0 = every cycle passed.
"""
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is needed: python -m pip install pyserial")

CMD_STATE, CMD_INFO, CMD_DIAG, CMD_RESTART, CMD_FAULT = 0x02, 0x03, 0x45, 0x44, 0x46
FAULTS = ("panic", "taskwdt", "intwdt", "deadlock", "leak")
START_RE = re.compile(r"supervisor: Start: ([^,]+), boot #(\d+), (\d+) restart")


def frame(cmd, args=b""):
    d = bytes([cmd, len(args)]) + args
    p = b"IMPROV" + bytes([1, 3, len(d)]) + d
    return p + bytes([sum(p) & 255]) + b"\n"


class Link:
    def __init__(self, port):
        # Windows releases a port a moment after the previous process closed it: retry briefly.
        for attempt in range(10):
            try:
                self.s = serial.Serial(port, 115200, timeout=0.2)
                break
            except serial.SerialException:
                if attempt == 9:
                    raise
                time.sleep(1.0)
        self.s.dtr = True
        self.s.rts = True            # both asserted = run (no reset, no download mode)
        self.buf = b""
        self.text = ""

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            d = self.s.read(4096)
            if d:
                self.buf += d
                self.text += d.decode("ascii", "replace")
                self.text = self.text[-60000:]

    def _result(self, cmd):
        """Pop the newest RPC result frame for cmd out of the buffer, or None."""
        i = 0
        found = None
        while True:
            i = self.buf.find(b"IMPROV", i)
            if i < 0 or i + 9 > len(self.buf):
                break
            ln = self.buf[i + 8]
            if i + 9 + ln + 2 > len(self.buf):
                break
            if self.buf[i + 7] == 4 and self.buf[i + 9] == cmd:
                d = self.buf[i + 9:i + 9 + ln]
                out, p = [], 2
                while p < 2 + d[1]:
                    out.append(d[p + 1:p + 1 + d[p]].decode("utf-8", "replace"))
                    p += 1 + d[p]
                found = out
            i += 9 + ln + 2
        return found

    def call(self, cmd, timeout=2.0):
        self.buf = b""
        self.s.write(frame(cmd))
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.15)
            r = self._result(cmd)
            if r is not None:
                return r
        return None

    def wait_alive(self, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.call(CMD_STATE, 1.2) is not None:
                return time.time() - t0
        return None

    def hard_reset(self):
        self.s.dtr = False
        self.s.rts = True
        time.sleep(0.15)
        self.s.rts = False
        time.sleep(0.05)
        self.s.dtr = True
        self.s.rts = True


def diag(link):
    r = link.call(CMD_DIAG, 3.0)
    if not r or len(r) < 11:
        return None
    k = ("reason boots streak crashes safe why task uptime heap heap_min largest").split()
    return dict(zip(k, r))


SV_REASON_RE = re.compile(r"restarted the bridge last time: reason (\d+) ?(\S*)")


def inject(link, kind):
    link.buf = b""
    link.s.write(frame(CMD_FAULT, bytes([10]) + b"CRASH-TEST" + bytes([len(kind)]) + kind.encode()))
    link.pump(0.8)


def wait_restart(link, boots_before, max_wait, settle_s=16):
    """Wait until the bridge has restarted (its start counter grew), then until it is settled again.

    A start is a Bluetooth-only boot followed by a normal one (two counts), unless it ends in safe mode
    (one count). "Settled" = it answers and has been up long enough to be past both.
    Returns (seconds until settled, diag) or (None, None).
    """
    t0 = time.time()
    d = None
    while time.time() - t0 < max_wait:
        r = link.call(CMD_DIAG, 1.5)
        if r and len(r) >= 11:
            d = dict(zip("reason boots streak crashes safe why task uptime heap heap_min largest".split(), r))
            if int(d["boots"]) > boots_before and int(d["uptime"]) >= settle_s:
                return time.time() - t0, d
        else:
            link.pump(0.5)
    return None, None


def run_faults(link, which, max_wait):
    """Inject each fault, wait for the board to restart AND settle by itself, and check what it recorded."""
    # fault -> (reason the FIRST start after it must report, does the crash counter grow, supervisor code or None)
    expect = {
        "panic":    ("Crash",              True,  None),
        "taskwdt":  ("Task watchdog",      True,  None),
        "intwdt":   ("Interrupt watchdog", True,  None),
        "deadlock": ("Software restart",   False, "1"),
        "leak":     ("Software restart",   False, "2"),
    }
    fails = 0
    print("%-9s %-9s %-20s %-9s %-7s %-12s %s" % ("fault", "settled", "first start says", "crashes", "streak", "supervisor", "verdict"))
    for kind in which:
        # a clean start first: the owner restarts, which also clears the loop counter
        link.call(CMD_RESTART, 3.0)
        if link.wait_alive(60) is None:
            print("%-9s the board did not come back from a plain restart" % kind)
            return fails + 1, None
        time.sleep(14)                       # past the Bluetooth boot
        d0 = diag(link)
        before_boots, before_crashes = int(d0["boots"]), int(d0["crashes"])
        link.text = ""
        inject(link, kind)
        took, d = wait_restart(link, before_boots, max_wait)
        starts = START_RE.findall(link.text)
        first = starts[0][0] if starts else "?"
        sv = SV_REASON_RE.findall(link.text)
        sv_code = sv[0][0] if sv else None
        want_reason, grows, want_sv = expect[kind]
        # Every injected fault is a failure, so the crash counter grows by exactly one for all of them
        # (a hang the supervisor ended counts like a crash the hardware ended).
        ok = d is not None and d["safe"] == "0" and first == want_reason and int(d["crashes"]) == before_crashes + 1
        if ok and want_sv:
            ok = sv_code == want_sv
        if not ok:
            fails += 1
        print("%-9s %-9s %-20s %-9s %-7s %-12s %s" % (
            kind, ("%.1fs" % took) if took is not None else "NEVER", first,
            (d or {}).get("crashes", "-"), (d or {}).get("streak", "-"),
            ("%s %s" % (sv[0][0], sv[0][1])).strip() if sv else "-", "ok" if ok else "FAIL"))
        sys.stdout.flush()
        if d is None:
            return fails, None
    return fails, d


def run_safe_mode(link, max_wait):
    """Three crashes in a row must end in safe mode; a restart from the owner must leave it."""
    fails = 0
    link.call(CMD_RESTART, 3.0)
    if link.wait_alive(60) is None:
        return 1, None
    time.sleep(14)
    d = diag(link)
    print("\nboot-loop check: three crashes in a row, none of them reaching the healthy mark")
    for i in range(1, 4):
        boots = int(d["boots"])
        link.text = ""
        inject(link, "panic")
        # the third crash ends in safe mode, which has no Bluetooth boot: it settles sooner
        took, d = wait_restart(link, boots, max_wait, settle_s=8)
        if d is None:
            print("  crash %d: the board never came back" % i)
            return fails + 1, None
        print("  crash %d: back after %.1fs, streak=%s safe=%s" % (i, took, d["streak"], d["safe"]))
    if d["safe"] != "1":
        print("  FAIL: not in safe mode after three crashes")
        fails += 1
    else:
        print("  ok: SAFE MODE entered, the console still answers")
    boots = int(d["boots"])
    link.call(CMD_RESTART, 3.0)
    took, d = wait_restart(link, boots, max_wait)
    if d is None or d["safe"] != "0" or d["streak"] != "0":
        print("  FAIL: a restart from the owner did not leave safe mode (%s)" % d)
        fails += 1
    else:
        print("  ok: a restart from the owner left safe mode (streak 0, back after %.1fs)" % took)
    return fails, d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--cycles", type=int, default=10)
    ap.add_argument("--mode", choices=["command", "hard", "fault"], default="command")
    ap.add_argument("--fault", choices=FAULTS, help="with --mode fault: only this one (default: all)")
    ap.add_argument("--max-wait", type=int, default=45, help="seconds a restart may take until USB answers (faults: the supervisor needs up to 100 s for a leak)")
    a = ap.parse_args()

    link = Link(a.port)
    up = link.wait_alive(60)
    if up is None:
        print("FAIL: no answer over USB at the start. Is it SpiderBridge firmware with the diagnostics command?")
        return 1
    first = diag(link)
    if not first:
        print("FAIL: the firmware does not answer the diagnostics command (0x45): too old.")
        return 1
    print("start: boots=%s crashes=%s streak=%s safe=%s heap=%s" %
          (first["boots"], first["crashes"], first["streak"], first["safe"], first["heap"]))

    if a.mode == "fault":
        which = (a.fault,) if a.fault else FAULTS
        fails, last = run_faults(link, which, a.max_wait)
        if not a.fault and last is not None:
            f2, _ = run_safe_mode(link, a.max_wait)
            fails += f2
        print("RESULT: %s" % ("PASS" if not fails else "FAIL"))
        return 1 if fails else 0

    print("%-4s %-8s %-10s %-20s %-7s %-7s %s" % ("#", "answer", "restart", "reset reason", "boots", "streak", "heap"))

    fails, times = 0, []
    base_crashes = int(first["crashes"])
    for n in range(1, a.cycles + 1):
        link.text = ""
        t0 = time.time()
        if a.mode == "command":
            link.call(CMD_RESTART, 3.0)
        else:
            link.hard_reset()
        back = link.wait_alive(a.max_wait)
        took = time.time() - t0
        d = diag(link) if back is not None else None
        m = START_RE.findall(link.text)
        reason = m[-1][0] if m else "?"
        ok = back is not None and d is not None and d["safe"] == "0" and int(d["crashes"]) == base_crashes
        if a.mode == "hard":
            ok = back is not None and d is not None and d["safe"] == "0"
        if not ok:
            fails += 1
        if back is not None:
            times.append(took)
        print("%-4d %-8s %-10s %-20s %-7s %-7s %s%s" % (
            n, ("%.1fs" % took) if back is not None else "NONE", a.mode, (d or {}).get("reason", reason),
            (d or {}).get("boots", "-"), (d or {}).get("streak", "-"), (d or {}).get("heap", "-"),
            "" if ok else "   <-- FAIL"))
        if d:
            base_crashes = int(d["crashes"]) if a.mode == "hard" else base_crashes
        sys.stdout.flush()

    if times:
        print("answer time: min %.1fs  avg %.1fs  max %.1fs" % (min(times), sum(times) / len(times), max(times)))
    print("RESULT: %s (%d of %d cycles passed)" % ("PASS" if not fails else "FAIL", a.cycles - fails, a.cycles))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())