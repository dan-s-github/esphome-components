#!/usr/bin/env python3
"""Check CURRENT_TIME (0x54) decodes in device logs against wall-clock time.

Does two things with crow_alarm_panel DEBUG logs as written by the frigate
crow-alarm-logger (each line prefixed with a UTC "YYYY-MM-DD HH:MM:SS" stamp):

1. Bit-unstuffing model check: for every 0x54 frame whose raw bytes were logged
   (only the glitched ones are), drop the 0 that follows five consecutive 1 bits
   (HDLC zero-insertion, LSB-first, see docs/protocol_wire_format.md "Frame Format")
   and check the result decodes to the wall-clock date and time.
2. Accepted-decode audit: compare every "Controller time update" line with the
   wall clock and list the ones that are off by more than a few minutes, i.e.
   stuffed frames that the decoder's date-halving recovery let through with a
   wrong time.

Usage:
    ssh frigate 'P=/opt/crow-alarm-logger/data/crow-alarm.log; \\
        sudo sh -c "zcat $P.4.gz $P.3.gz $P.2.gz; cat $P.1 $P"' \\
      | grep -E "Controller time update|Current time" > time.log
    uv run python tools/analyze_current_time.py time.log
    uv run python tools/analyze_current_time.py --utc-offset 12 old.log.gz   # NZST capture

Firmware from 2026-10-29 on removes stuffed bits in the ISR. On its logs, expect no raw
frames at all and 0 bad decodes; any raw frame that does appear is genuine corruption
already un-stuffed, so its "model miss" is expected and the bytes are what to look at.

Log line prefixes are UTC; the panel clock is local time, hence --utc-offset
(13 = NZDT, 12 = NZST). A capture spanning a DST change will show a one-hour
offset bucket for part of the window.
"""
import argparse
import gzip
import re
import sys
from collections import Counter
from datetime import datetime, timedelta

LINE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) \[[^\]]+\]\[\w\]\[[^\]]+\]: \[Controller +\] (.*)$")
UPDATE = re.compile(r"Controller time update: \w+ (\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+)")
RAW = re.compile(r"\[54\.([0-9A-Fa-f.]+) \(7\)\]")


def hexs(bs):
    return ".".join(f"{b:02X}" for b in bs)


def unstuff(frame):
    """Return (frame with stuffed bits removed, number of stuffed bits).

    Same rule as CrowAlarmPanelStore::interrupt(): drop a 0 that follows exactly five 1s.
    """
    out, run, stuffed = [], 0, 0
    for b in frame:
        for i in range(8):
            bit = (b >> i) & 1
            if bit:
                run = min(run + 1, 6)
            elif run == 5:
                run = 0
                stuffed += 1
                continue
            else:
                run = 0
            out.append(bit)
    # The receiver framed only the first 64 bits, so the bits pushed past the end by
    # stuffing are lost; they are the year's high bits, which are 0 for any year < 2128.
    out += [0] * stuffed
    return [sum(out[i + j] << j for j in range(8)) for i in range(0, len(out) - 7, 8)], stuffed


def open_log(path):
    if path == "-":
        return sys.stdin
    if path.endswith(".gz"):
        return gzip.open(path, "rt", errors="replace")
    return open(path, errors="replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logs", nargs="+", help="log files (.gz ok, '-' for stdin), in time order")
    ap.add_argument("--utc-offset", type=float, default=13, help="panel local time minus UTC, hours (default 13)")
    ap.add_argument("--tolerance", type=int, default=5, help="minutes of clock drift to ignore (default 5)")
    ap.add_argument("--show", type=int, default=20, help="max distinct bad decodes / model misses to list")
    args = ap.parse_args()
    utc_offset = timedelta(hours=args.utc_offset)

    raws = []  # (utc timestamp, raw frame)
    updates = []  # (utc timestamp, decoded datetime, raw frame of the same broadcast or None)
    pending_raw = None
    for path in args.logs:
        for line in open_log(path):
            m = LINE.match(line)
            if not m:
                continue
            ts = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
            msg = m.group(2)
            r = RAW.search(msg)
            if r:
                frame = [0x54] + [int(x, 16) for x in r.group(1).split(".")]
                if pending_raw is None or pending_raw[1] != frame:
                    raws.append((ts, frame))
                pending_raw = (ts, frame)
                continue
            u = UPDATE.search(msg)
            if u:
                y, mo, d, hh, mi, ss = map(int, u.groups())
                # A raw line within a couple of seconds belongs to this broadcast (the two lines
                # can straddle a second boundary); otherwise this frame decoded cleanly.
                raw = pending_raw[1] if pending_raw and (ts - pending_raw[0]).total_seconds() <= 2 else None
                updates.append((ts, datetime(y, mo, d, hh, mi, ss), raw))  # logged as "2026-..."
            pending_raw = None

    # 1. Model check.
    misses = []
    for ts, frame in raws:
        un, _ = unstuff(frame)
        _, mins_hi, mins_lo, sec, day, mon, yr = un[1:8]
        mins = (mins_hi << 8) | mins_lo
        local = ts + utc_offset
        ok = mins < 1440 and sec < 60 and 1 <= day <= 31 and 1 <= mon <= 12
        if ok:
            dec = datetime(2000 + yr, mon, day) + timedelta(minutes=mins, seconds=sec)
            ok = abs((dec - local).total_seconds()) <= 15 * 60
        if not ok:
            misses.append((ts, frame, un))
    print(f"raw 0x54 frames: {len(raws)}; unstuffed decode matches wall clock (±15 min): "
          f"{len(raws) - len(misses)}, misses: {len(misses)}")
    for ts, frame, un in misses[: args.show]:
        print(f"  MISS {ts} UTC raw {hexs(frame)} unstuffed {hexs(un)}")

    # 2. Accepted-decode audit.
    offsets = Counter()
    bad = []
    for ts, dec, raw in updates:
        off = round((dec - (ts + utc_offset)).total_seconds() / 60)
        offsets[off] += 1
        if abs(off) > args.tolerance:
            bad.append((ts, dec, off, raw))
    print(f"\naccepted 'Controller time update' lines: {len(updates)}")
    print("offset (decoded - wall clock), minutes:", offsets.most_common(8))
    print(f"off by more than {args.tolerance} min: {len(bad)}  by offset: {Counter(o for *_, o, _ in bad).most_common()}")
    seen = set()
    for ts, dec, off, raw in bad:
        key = (off, raw[3] if raw else None)
        if key in seen:
            continue
        seen.add(key)
        if len(seen) > args.show:
            break
        detail = f"raw {hexs(raw)} unstuffed {hexs(unstuff(raw)[0])}" if raw else "(no raw bytes logged)"
        print(f"  {ts} UTC decoded {dec:%H:%M:%S} off {off:+d}m {detail}")


if __name__ == "__main__":
    main()
