#!/usr/bin/env python3
"""
dvbt_tune.py - tune a DVB-T multiplex on the H727 and report lock/signal.

Usage: dvbt_tune.py [freq_MHz ...] [--adapter N] [--bw 7|8] [--wait SECONDS]
With no frequencies, tries the main Melbourne (Mt Dandenong) multiplexes.

Uses the Linux DVBv5 property API directly, so no dvb-tools are needed.
"""
import argparse
import ctypes
import fcntl
import os
import struct
import time

FE_READ_STATUS = 0x80046f45
FE_READ_SIGNAL_STRENGTH = 0x80026f47
FE_READ_SNR = 0x80026f48
FE_READ_BER = 0x80046f46
FE_SET_PROPERTY = 0x40106f52

DTV_TUNE, DTV_CLEAR, DTV_FREQUENCY, DTV_MODULATION = 1, 2, 3, 4
DTV_BANDWIDTH_HZ, DTV_INVERSION, DTV_DELIVERY_SYSTEM = 5, 6, 17
DTV_CODE_RATE_HP, DTV_CODE_RATE_LP, DTV_GUARD_INTERVAL = 36, 37, 38
DTV_TRANSMISSION_MODE, DTV_HIERARCHY = 39, 40
SYS_DVBT = 3
QAM_AUTO, INVERSION_AUTO, FEC_AUTO = 6, 2, 9
GUARD_INTERVAL_AUTO, TRANSMISSION_MODE_AUTO, HIERARCHY_AUTO = 4, 2, 4

MELBOURNE = [(177.5, "Seven"), (184.5, "SBS"), (191.625, "Nine"),
             (219.5, "Ten"), (226.5, "ABC")]

STATUS_BITS = [(0x01, "SIGNAL"), (0x02, "CARRIER"), (0x04, "VITERBI"),
               (0x08, "SYNC"), (0x10, "LOCK"), (0x20, "TIMEDOUT")]


class dtv_property(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("cmd", ctypes.c_uint32), ("reserved", ctypes.c_uint32 * 3),
                ("data", ctypes.c_uint8 * 56), ("result", ctypes.c_int32)]


class dtv_properties(ctypes.Structure):
    _fields_ = [("num", ctypes.c_uint32), ("props", ctypes.POINTER(dtv_property))]


def set_props(fd, pairs):
    arr = (dtv_property * len(pairs))()
    for i, (cmd, val) in enumerate(pairs):
        arr[i].cmd = cmd
        struct.pack_into("<I", arr[i].data, 0, val)
    req = dtv_properties(len(pairs), arr)
    fcntl.ioctl(fd, FE_SET_PROPERTY, req)


def read_u(fd, req, fmt):
    buf = bytearray(struct.calcsize(fmt))
    try:
        fcntl.ioctl(fd, req, buf)
        return struct.unpack(fmt, buf)[0]
    except OSError:
        return None


def tune(fd, freq_mhz, bw_hz):
    set_props(fd, [(DTV_CLEAR, 0)])
    set_props(fd, [
        (DTV_DELIVERY_SYSTEM, SYS_DVBT),
        (DTV_FREQUENCY, int(round(freq_mhz * 1e6))),
        (DTV_BANDWIDTH_HZ, bw_hz),
        (DTV_MODULATION, QAM_AUTO),
        (DTV_INVERSION, INVERSION_AUTO),
        (DTV_CODE_RATE_HP, FEC_AUTO),
        (DTV_CODE_RATE_LP, FEC_AUTO),
        (DTV_GUARD_INTERVAL, GUARD_INTERVAL_AUTO),
        (DTV_TRANSMISSION_MODE, TRANSMISSION_MODE_AUTO),
        (DTV_HIERARCHY, HIERARCHY_AUTO),
        (DTV_TUNE, 0),
    ])


def describe(st):
    if st is None:
        return "status unavailable"
    names = [n for b, n in STATUS_BITS if st & b]
    return "|".join(names) if names else "none"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("freqs", nargs="*", type=float)
    ap.add_argument("--adapter", type=int, default=0)
    ap.add_argument("--bw", type=int, default=7)
    ap.add_argument("--wait", type=float, default=4.0)
    ap.add_argument("--hold", type=float, default=0,
                    help="keep the frontend open (stay tuned to the last frequency) for N seconds")
    a = ap.parse_args()
    targets = [(f, "") for f in a.freqs] or MELBOURNE
    path = f"/dev/dvb/adapter{a.adapter}/frontend0"
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    try:
        for freq, name in targets:
            tune(fd, freq, a.bw * 1000000)
            t0, st = time.time(), 0
            while time.time() - t0 < a.wait:
                st = read_u(fd, FE_READ_STATUS, "<I") or 0
                if st & 0x10:
                    break
                time.sleep(0.2)
            sig = read_u(fd, FE_READ_SIGNAL_STRENGTH, "<H")
            snr = read_u(fd, FE_READ_SNR, "<H")
            ber = read_u(fd, FE_READ_BER, "<I")
            label = f"{freq:8.3f} MHz {name:6s}"
            print(f"{label} status={describe(st):40s} signal={sig} snr={snr} ber={ber} "
                  f"({time.time() - t0:.1f}s)", flush=True)
        if a.hold:
            end = time.time() + a.hold
            while time.time() < end:
                time.sleep(5)
                st = read_u(fd, FE_READ_STATUS, "<I") or 0
                print(f"  holding: {describe(st)}", flush=True)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
