#!/usr/bin/env python3
"""
tm62reg - live register access for the AVerMedia H727 (TM6202) while avertm62 is loaded.

BAR0 (mmap of the PCI resource0 file):
  tm62reg r   <off> [count]            read 32-bit register(s)
  tm62reg w   <off> <val>              write 32-bit register
  tm62reg bf  <off> <lsb> <width> <v>  read-modify-write a bitfield
  tm62reg dump [start] [end]           hex dump of a register range (default 0x000-0x7ff)

I2C, through the driver's own adapters (needs the i2c-dev module):
  tm62reg ir  <bus> <addr8> <reg> [count]   read byte(s), addr8 is the 8-bit address (e.g. 0x60)
  tm62reg iw  <bus> <addr8> <reg> <val>     write a byte

Must run as root. Install a root-owned copy (see the setup notes) rather than
running this file from a user-writable directory with sudo.
"""
import ctypes
import fcntl
import glob
import mmap
import os
import struct
import sys

VENDOR, DEVICES = 0x1a0a, (0x6200, 0x6202)
BAR0_SIZE = 0x8000


def find_pci():
    for d in glob.glob("/sys/bus/pci/devices/*"):
        try:
            v = int(open(d + "/vendor").read(), 16)
            p = int(open(d + "/device").read(), 16)
        except OSError:
            continue
        if v == VENDOR and p in DEVICES:
            return d
    sys.exit("TM6202 not found on the PCI bus")


def bar0():
    fd = os.open(find_pci() + "/resource0", os.O_RDWR | os.O_SYNC)
    return mmap.mmap(fd, BAR0_SIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)


def check_off(off):
    if off < 0 or off >= BAR0_SIZE or off & 3:
        sys.exit(f"bad offset 0x{off:x}: must be 4-byte aligned and below 0x{BAR0_SIZE:x}")


def rd(m, off):
    check_off(off)
    return struct.unpack_from("<I", m, off)[0]


def wr(m, off, val):
    check_off(off)
    struct.pack_into("<I", m, off, val & 0xffffffff)


# --- I2C via /dev/i2c-N, combined write+read with I2C_RDWR ---
I2C_RDWR = 0x0707
I2C_M_RD = 0x0001


class i2c_msg(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("len", ctypes.c_uint16), ("buf", ctypes.POINTER(ctypes.c_uint8))]


class i2c_rdwr_ioctl_data(ctypes.Structure):
    _fields_ = [("msgs", ctypes.POINTER(i2c_msg)), ("nmsgs", ctypes.c_uint32)]


def i2c_dev(bus):
    for d in glob.glob("/sys/bus/i2c/devices/i2c-*") + glob.glob("/sys/class/i2c-adapter/i2c-*"):
        try:
            if open(d + "/name").read().strip() == f"avertm62-i2c{bus}":
                return "/dev/" + os.path.basename(d)
        except OSError:
            pass
    sys.exit(f"avertm62-i2c{bus} adapter not found (driver loaded? i2c-dev loaded?)")


def i2c_xfer(bus, addr7, msgs_spec):
    path = i2c_dev(bus)
    if not os.path.exists(path):
        sys.exit(f"{path} missing: run 'modprobe i2c-dev'")
    fd = os.open(path, os.O_RDWR)
    try:
        bufs, msgs = [], (i2c_msg * len(msgs_spec))()
        for i, (flags, data) in enumerate(msgs_spec):
            b = (ctypes.c_uint8 * len(data))(*data)
            bufs.append(b)
            msgs[i] = i2c_msg(addr7, flags, len(data), b)
        req = i2c_rdwr_ioctl_data(msgs, len(msgs_spec))
        fcntl.ioctl(fd, I2C_RDWR, req)
        return [bytes(b) for b in bufs]
    finally:
        os.close(fd)


def num(s):
    return int(s, 0)


def main(a):
    if len(a) < 1:
        sys.exit(__doc__)
    cmd = a[0]
    if cmd == "r":
        m, off = bar0(), num(a[1])
        for i in range(num(a[2]) if len(a) > 2 else 1):
            print(f"0x{off + 4 * i:04x} = 0x{rd(m, off + 4 * i):08x}")
    elif cmd == "w":
        m, off, val = bar0(), num(a[1]), num(a[2])
        old = rd(m, off)
        wr(m, off, val)
        print(f"0x{off:04x}: 0x{old:08x} -> 0x{val:08x} (now 0x{rd(m, off):08x})")
    elif cmd == "bf":
        m, off, lsb, w, v = bar0(), num(a[1]), num(a[2]), num(a[3]), num(a[4])
        mask = (((1 << w) - 1) << lsb) & 0xffffffff
        old = rd(m, off)
        new = (old & ~mask) | ((v << lsb) & mask)
        wr(m, off, new)
        print(f"0x{off:04x}[{lsb + w - 1}:{lsb}]={v}: 0x{old:08x} -> 0x{new:08x} (now 0x{rd(m, off):08x})")
    elif cmd == "dump":
        m = bar0()
        start = num(a[1]) if len(a) > 1 else 0
        end = num(a[2]) if len(a) > 2 else 0x7fc
        for off in range(start & ~0xf, end + 1, 16):
            vals = " ".join(f"{rd(m, off + 4 * i):08x}" for i in range(4) if off + 4 * i <= end)
            print(f"0x{off:04x}: {vals}")
    elif cmd == "ir":
        bus, addr8, reg = num(a[1]), num(a[2]), num(a[3])
        n = num(a[4]) if len(a) > 4 else 1
        _, data = i2c_xfer(bus, addr8 >> 1, [(0, [reg]), (I2C_M_RD, [0] * n)])
        for i, b in enumerate(data):
            print(f"bus{bus} 0x{addr8:02x}:0x{reg + i:02x} = 0x{b:02x}")
    elif cmd == "iw":
        bus, addr8, reg, val = num(a[1]), num(a[2]), num(a[3]), num(a[4])
        i2c_xfer(bus, addr8 >> 1, [(0, [reg, val & 0xff])])
        print(f"bus{bus} 0x{addr8:02x}:0x{reg:02x} <- 0x{val & 0xff:02x}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
