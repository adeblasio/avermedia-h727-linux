#!/usr/bin/env python3
"""
dvbt_scan.py - scan DVB-T multiplexes on the H727 and save the channel list.

Usage: dvbt_scan.py [freq_MHz ...] [--out FILE] [--seconds N] [--adapter N] [--bw 7|8]

With no frequencies, scans the Melbourne (Mt Dandenong) list from dvbt_tune.py.
Each multiplex is tuned, its SDT is read from dvr0 and every service is saved
as {"name", "provider", "freq_mhz", "service_id", "type"} in a JSON list
(default ~/.config/h727/channels.json), which the viewer uses.
"""
import argparse
import fcntl
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dvbt_tune import FE_READ_STATUS, MELBOURNE, read_u, tune  # noqa: E402

DMX_SET_PES_FILTER = 0x40146F2C
DMX_SET_BUFFER_SIZE = 0x6F2D
DMX_STOP = 0x6F2A
SDT_PID = 0x11
DEFAULT_OUT = os.path.expanduser("~/.config/h727/channels.json")
SERVICE_TYPES = {0x01: "tv", 0x02: "radio", 0x0A: "radio", 0x11: "tv",
                 0x16: "tv", 0x19: "tv", 0x1F: "tv"}


def take_section(sec, sections):
    """Store a complete SDT-actual section; return all of them once complete."""
    if len(sec) >= 8 and sec[0] == 0x42:
        sections[sec[6]] = sec                  # keyed by section_number
        if len(sections) > sec[7]:              # have 0..last_section_number
            return list(sections.values())
    return None


def read_sdt(base, seconds):
    """Collect complete SDT-actual sections (table 0x42) from the SDT PID."""
    dmx = os.open(f"{base}/demux0", os.O_RDWR)
    fcntl.ioctl(dmx, DMX_SET_BUFFER_SIZE, 1024 * 1024)
    # PID 0x11 only, TS packets to the dvr device, start immediately
    fcntl.ioctl(dmx, DMX_SET_PES_FILTER, struct.pack("<HxxIIII", SDT_PID, 0, 2, 20, 4))
    dvr = os.open(f"{base}/dvr0", os.O_RDONLY | os.O_NONBLOCK)
    sections, buf, pending = {}, b"", b""
    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            try:
                data = os.read(dvr, 188 * 64)
            except BlockingIOError:
                time.sleep(0.05)
                continue
            data = pending + data
            n = len(data) // 188
            pending = data[n * 188:]
            for k in range(n):
                p = data[k * 188:(k + 1) * 188]
                if p[0] != 0x47 or p[1] & 0x80:      # lost sync / transport error
                    buf = b""
                    continue
                afc = (p[3] >> 4) & 3
                if not afc & 1:
                    continue
                payload = p[4:] if afc == 1 else p[5 + p[4]:]
                if p[1] & 0x40:                 # a section starts in this packet
                    ptr = payload[0]
                    if buf:
                        buf += payload[1:1 + ptr]   # tail of the previous one
                        done = take_section(buf, sections)
                        if done:
                            return done
                    buf = payload[1 + ptr:]
                elif buf:
                    buf += payload
                else:
                    continue
                while len(buf) >= 3 and buf[0] != 0xFF:
                    length = 3 + (((buf[1] & 0x0F) << 8) | buf[2])
                    if len(buf) < length:
                        break
                    done = take_section(buf[:length], sections)
                    if done:
                        return done
                    buf = buf[length:]
                if buf[:1] == b"\xff":
                    buf = b""
    finally:
        os.close(dvr)
        os.close(dmx)
    return list(sections.values())


def parse_sdt(sec):
    out = []
    end = len(sec) - 4
    i = 11
    while i + 5 <= end:
        sid = (sec[i] << 8) | sec[i + 1]
        dlen = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4]
        j, dend = i + 5, min(i + 5 + dlen, end)
        while j + 2 <= dend:
            tag, tl = sec[j], sec[j + 1]
            d = sec[j + 2:j + 2 + tl]
            if tag == 0x48 and len(d) >= 3:
                stype, pl = d[0], d[1]
                prov = d[2:2 + pl]
                if 2 + pl < len(d):
                    nl = d[2 + pl]
                    name = d[3 + pl:3 + pl + nl]
                    out.append({"service_id": sid,
                                "name": name.decode("latin-1").strip("\x00 "),
                                "provider": prov.decode("latin-1").strip("\x00 "),
                                "type": SERVICE_TYPES.get(stype, f"0x{stype:02x}")})
            j += 2 + tl
        i += 5 + dlen
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("freqs", nargs="*", type=float)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--seconds", type=float, default=20,
                    help="max time to wait for the SDT per multiplex")
    ap.add_argument("--adapter", type=int, default=0)
    ap.add_argument("--bw", type=int, default=7)
    a = ap.parse_args()
    base = f"/dev/dvb/adapter{a.adapter}"
    freqs = a.freqs or [f for f, _ in MELBOURNE]

    channels = []
    fe = os.open(f"{base}/frontend0", os.O_RDWR | os.O_NONBLOCK)
    try:
        for freq in freqs:
            tune(fe, freq, a.bw * 1000000)
            t0, st = time.time(), 0
            while time.time() - t0 < 4:
                st = read_u(fe, FE_READ_STATUS, "<I") or 0
                if st & 0x10:
                    break
                time.sleep(0.1)
            if not st & 0x10:
                print(f"{freq:8.3f} MHz: no lock")
                continue
            services = []
            for sec in read_sdt(base, a.seconds):
                services += parse_sdt(sec)
            services.sort(key=lambda s: s["service_id"])
            print(f"{freq:8.3f} MHz: {len(services)} services")
            for s in services:
                s["freq_mhz"] = freq
                print(f"    {s['service_id']:5d} {s['type']:5s} {s['name']}")
            channels += services
    finally:
        os.close(fe)

    if channels:
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        with open(a.out, "w") as f:
            json.dump(channels, f, indent=1)
        print(f"saved {len(channels)} channels to {a.out}")


if __name__ == "__main__":
    main()
