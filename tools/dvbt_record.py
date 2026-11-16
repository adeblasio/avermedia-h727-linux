#!/usr/bin/env python3
"""
dvbt_record.py - tune a DVB-T multiplex on the H727 and record its transport
stream from /dev/dvb/adapterN/dvr0.

Usage: dvbt_record.py FREQ_MHz [--seconds N] [--out FILE] [--adapter N] [--bw 7|8]

Prints the data rate, sync/continuity statistics and the services listed in
the multiplex's SDT. Without --out the stream is only analysed.
Play a recording with e.g.:  mpv --demuxer-lavf-format=mpegts out.ts
"""
import argparse
import collections
import fcntl
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dvbt_tune import FE_READ_STATUS, describe, read_u, tune  # noqa: E402

DMX_SET_PES_FILTER = 0x40146F2C
DMX_SET_BUFFER_SIZE = 0x6F2D
DMX_IN_FRONTEND, DMX_OUT_TS_TAP, DMX_PES_OTHER, DMX_IMMEDIATE_START = 0, 2, 20, 4
ALL_PIDS = 0x2000


def sdt_services(sections):
    """Parse SDT (table 0x42) sections -> {service_id: (provider, name)}."""
    out = {}
    for sec in sections:
        if len(sec) < 11 or sec[0] != 0x42:
            continue
        length = ((sec[1] & 0x0F) << 8) | sec[2]
        end = 3 + length - 4
        i = 11
        end = min(end, len(sec))
        while i + 5 <= end:
            sid = (sec[i] << 8) | sec[i + 1]
            dlen = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4]
            j, dend = i + 5, i + 5 + dlen
            while j + 2 <= dend:
                tag, tl = sec[j], sec[j + 1]
                if tag == 0x48 and j + 2 + tl <= min(dend, len(sec)) and tl >= 3:
                    d = sec[j + 2:j + 2 + tl]
                    pl = d[1]
                    prov = d[2:2 + pl]
                    nl = d[2 + pl]
                    name = d[3 + pl:3 + pl + nl]
                    if nl > len(name):
                        break
                    out[sid] = (prov.decode("latin-1", "replace").strip("\x00"),
                                name.decode("latin-1", "replace").strip("\x00"))
                j += 2 + tl
            i = dend
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("freq", type=float)
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--out")
    ap.add_argument("--adapter", type=int, default=0)
    ap.add_argument("--bw", type=int, default=7)
    ap.add_argument("--verbose", action="store_true", help="print the data rate every second")
    a = ap.parse_args()

    base = f"/dev/dvb/adapter{a.adapter}"
    fe = os.open(f"{base}/frontend0", os.O_RDWR | os.O_NONBLOCK)
    tune(fe, a.freq, a.bw * 1000000)
    t0 = time.time()
    st = 0
    while time.time() - t0 < 5:
        st = read_u(fe, FE_READ_STATUS, "<I") or 0
        if st & 0x10:
            break
        time.sleep(0.1)
    print(f"{a.freq:.3f} MHz: {describe(st)}", flush=True)
    if not st & 0x10:
        sys.exit("no lock")

    dmx = os.open(f"{base}/demux0", os.O_RDWR)
    fcntl.ioctl(dmx, DMX_SET_BUFFER_SIZE, 4 * 1024 * 1024)
    fcntl.ioctl(dmx, DMX_SET_PES_FILTER, struct.pack(
        "<HxxIIII", ALL_PIDS, DMX_IN_FRONTEND, DMX_OUT_TS_TAP, DMX_PES_OTHER,
        DMX_IMMEDIATE_START))
    dvr = os.open(f"{base}/dvr0", os.O_RDONLY)
    out = open(a.out, "wb") if a.out else None

    total, pkts, bad_sync, cc_err = 0, 0, 0, 0
    pids = collections.Counter()
    last_cc = {}
    sdt_buf, sdt_secs = b"", []
    pending = b""
    t0 = time.time()
    tick, tick_bytes = t0 + 1, 0
    while time.time() - t0 < a.seconds:
        data = os.read(dvr, 188 * 1024)
        if a.verbose and time.time() >= tick:
            print(f"  {tick - t0:4.0f}s {(total - tick_bytes) * 8 / 1e6:6.2f} Mbit/s", flush=True)
            tick, tick_bytes = tick + 1, total
        if not data:
            continue
        total += len(data)
        if out:
            out.write(data)
        data = pending + data
        n = len(data) // 188
        pending = data[n * 188:]
        for k in range(n):
            p = data[k * 188:(k + 1) * 188]
            pkts += 1
            if p[0] != 0x47:
                bad_sync += 1
                continue
            pid = ((p[1] & 0x1F) << 8) | p[2]
            pids[pid] += 1
            if pid == 0x1FFF:
                continue
            afc = (p[3] >> 4) & 3
            cc = p[3] & 0x0F
            if afc & 1:
                if pid in last_cc and cc != (last_cc[pid] + 1) & 0x0F and cc != last_cc[pid]:
                    cc_err += 1
                last_cc[pid] = cc
            if pid == 0x11 and len(sdt_secs) < 20 and afc & 1 and not p[1] & 0x80:
                payload = p[4:] if afc == 1 else p[5 + p[4]:]
                if p[1] & 0x40:          # payload unit start: pointer field
                    if sdt_buf:
                        sdt_secs.append(sdt_buf)
                    sdt_buf = payload[1 + payload[0]:]
                elif sdt_buf:
                    sdt_buf += payload
    elapsed = time.time() - t0
    for fd in (dvr, dmx, fe):
        os.close(fd)
    if out:
        out.close()

    print(f"read {total} bytes in {elapsed:.1f}s = {total * 8 / elapsed / 1e6:.2f} Mbit/s, "
          f"{pkts} packets, bad sync {bad_sync}, continuity errors {cc_err}")
    print("busiest PIDs: " + ", ".join(f"0x{p:04x}:{c}" for p, c in pids.most_common(8)))
    try:
        services = sdt_services(sdt_secs)
    except IndexError:          # a corrupted section (reception errors)
        services = {}
    for sid, (prov, name) in sorted(services.items()):
        print(f"  service {sid:5d}: {name} ({prov})")
    if a.out:
        print(f"saved to {a.out}")


if __name__ == "__main__":
    main()
