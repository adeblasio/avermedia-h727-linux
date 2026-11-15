#!/usr/bin/env python3
"""
Corrected 64-bit capture test script for AVerMedia H727 (/dev/video0)
"""
import sys
import os
import fcntl
import mmap
import ctypes
import time

VIDIOC_QUERYCAP = 0x80685600
VIDIOC_G_FMT = 0xc0d05604
VIDIOC_S_FMT = 0xc0d05605
VIDIOC_REQBUFS = 0xc0145608
VIDIOC_QUERYBUF = 0xc0585609
VIDIOC_QBUF = 0xc058560f
VIDIOC_DQBUF = 0xc0585611
VIDIOC_STREAMON = 0x40045612
VIDIOC_STREAMOFF = 0x40045613
VIDIOC_LOG_STATUS = 0x5646

V4L2_BUF_TYPE_VIDEO_CAPTURE = 1
V4L2_MEMORY_MMAP = 1
V4L2_PIX_FMT_YUYV = 0x56595559  # 'YUYV'

# Exact 64-bit Linux kernel struct v4l2_format definition
class v4l2_pix_format(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("pixelformat", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("bytesperline", ctypes.c_uint32),
        ("sizeimage", ctypes.c_uint32),
        ("colorspace", ctypes.c_uint32),
        ("priv", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("ycbcr_enc", ctypes.c_uint32),
        ("quantization", ctypes.c_uint32),
        ("xfer_func", ctypes.c_uint32),
    ]

class v4l2_format(ctypes.Structure):
    class _u(ctypes.Union):
        _fields_ = [
            ("pix", v4l2_pix_format),
            ("raw_data", ctypes.c_uint8 * 200),
        ]
    _anonymous_ = ("_u",)
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("_pad", ctypes.c_uint32), # 64-bit union alignment padding
        ("_u", _u),
    ]

# Exact 64-bit Linux kernel struct v4l2_buffer definition
class timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_usec", ctypes.c_long)]

class timecode(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("frames", ctypes.c_uint8),
        ("seconds", ctypes.c_uint8),
        ("minutes", ctypes.c_uint8),
        ("hours", ctypes.c_uint8),
        ("userbits", ctypes.c_uint8 * 4),
    ]

class v4l2_buffer(ctypes.Structure):
    class _m(ctypes.Union):
        _fields_ = [
            ("offset", ctypes.c_uint32),
            ("userptr", ctypes.c_ulong),
            ("planes", ctypes.c_void_p),
            ("fd", ctypes.c_int32),
        ]
    _anonymous_ = ("_m",)
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("bytesused", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("timestamp", timeval),
        ("timecode", timecode),
        ("sequence", ctypes.c_uint32),
        ("memory", ctypes.c_uint32),
        ("_m", _m),
        ("length", ctypes.c_uint32),
        ("reserved2", ctypes.c_uint32),
        ("request_fd", ctypes.c_int32),
    ]

class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("memory", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 2),
    ]

def main():
    dev_path = "/dev/video0"
    if not os.path.exists(dev_path):
        print(f"Error: {dev_path} not found.")
        sys.exit(1)

    print(f"Opening {dev_path}...")
    fd = os.open(dev_path, os.O_RDWR | os.O_NONBLOCK)

    # 1. Log hardware status
    print("Calling VIDIOC_LOG_STATUS...")
    try:
        fcntl.ioctl(fd, VIDIOC_LOG_STATUS)
    except Exception as e:
        print(f"VIDIOC_LOG_STATUS note: {e}")

    # 2. Set format (support width and height argument or default to 1920x1080)
    w = int(sys.argv[1]) if len(sys.argv) > 1 else 1920
    h = int(sys.argv[2]) if len(sys.argv) > 2 else 1080
    fmt = v4l2_format()
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
    fmt.pix.width = w
    fmt.pix.height = h
    fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV
    fmt.pix.field = 1 # V4L2_FIELD_NONE
    fmt.pix.bytesperline = w * 2
    fmt.pix.sizeimage = w * h * 2

    try:
        fcntl.ioctl(fd, VIDIOC_S_FMT, fmt)
        print(f"VIDIOC_S_FMT configured: {fmt.pix.width}x{fmt.pix.height} YUYV ({fmt.pix.sizeimage} bytes/frame)")
    except Exception as e:
        print(f"VIDIOC_S_FMT error: {e}")

    # 3. Request 4 MMIO buffers
    req = v4l2_requestbuffers()
    req.count = 4
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
    req.memory = V4L2_MEMORY_MMAP
    fcntl.ioctl(fd, VIDIOC_REQBUFS, req)
    print(f"Allocated {req.count} V4L2 buffers")

    buffers = []
    for i in range(req.count):
        qbuf = v4l2_buffer()
        qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
        qbuf.memory = V4L2_MEMORY_MMAP
        qbuf.index = i
        fcntl.ioctl(fd, VIDIOC_QUERYBUF, qbuf)

        buf_len = qbuf.length
        buf_off = qbuf.offset
        print(f"  Buffer {i}: length={buf_len} offset={hex(buf_off)}")

        buf_mmap = mmap.mmap(fd, buf_len, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=buf_off)
        buffers.append(buf_mmap)

        # Queue buffer
        fcntl.ioctl(fd, VIDIOC_QBUF, qbuf)

    # 4. Stream ON
    buf_type = ctypes.c_uint32(V4L2_BUF_TYPE_VIDEO_CAPTURE)
    fcntl.ioctl(fd, VIDIOC_STREAMON, buf_type)
    print("STREAMON succeeded! Capturing frames...")

    captured = 0
    t0 = time.time()
    last_frame_bytes = None

    while captured < 5 and (time.time() - t0) < 10.0:
        dq = v4l2_buffer()
        dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
        dq.memory = V4L2_MEMORY_MMAP
        try:
            fcntl.ioctl(fd, VIDIOC_DQBUF, dq)
            idx = dq.index
            bytesused = dq.bytesused
            seq = dq.sequence

            # Read full frame
            last_frame_bytes = buffers[idx][:bytesused]
            captured += 1
            print(f"  [Frame {captured}] idx={idx} bytesused={bytesused} seq={seq}")

            # Re-queue
            fcntl.ioctl(fd, VIDIOC_QBUF, dq)
        except BlockingIOError:
            time.sleep(0.016)

    # Call VIDIOC_LOG_STATUS right before STREAMOFF to see active DMA slots
    try:
        fcntl.ioctl(fd, VIDIOC_LOG_STATUS)
    except Exception:
        pass

    # 5. Stream OFF
    fcntl.ioctl(fd, VIDIOC_STREAMOFF, buf_type)
    print("STREAMOFF complete.")

    for b in buffers:
        b.close()
    os.close(fd)

    if last_frame_bytes:
        out_raw = "/tmp/avertm62_frame.yuv"
        with open(out_raw, "wb") as f:
            f.write(last_frame_bytes)
        print(f"Successfully saved captured frame ({len(last_frame_bytes)} bytes) to {out_raw}")

        # Also save a PPM image for immediate viewing
        try:
            ppm_path = "/tmp/avertm62_frame.ppm"
            # Quick YUYV to RGB conversion for 1920x1080
            rgb = bytearray(w * h * 3)
            out_i = 0
            for i in range(0, min(len(last_frame_bytes), w * h * 2), 4):
                y0 = last_frame_bytes[i]
                u  = last_frame_bytes[i+1] - 128
                y1 = last_frame_bytes[i+2]
                v  = last_frame_bytes[i+3] - 128

                r0 = max(0, min(255, int(y0 + 1.402 * v)))
                g0 = max(0, min(255, int(y0 - 0.344136 * u - 0.714136 * v)))
                b0 = max(0, min(255, int(y0 + 1.772 * u)))

                r1 = max(0, min(255, int(y1 + 1.402 * v)))
                g1 = max(0, min(255, int(y1 - 0.344136 * u - 0.714136 * v)))
                b1 = max(0, min(255, int(y1 + 1.772 * u)))

                rgb[out_i:out_i+6] = bytes([r0, g0, b0, r1, g1, b1])
                out_i += 6

            with open(ppm_path, "wb") as f:
                f.write(f"P6\n{w} {h}\n255\n".encode())
                f.write(rgb)
            print(f"Successfully saved converted preview image to {ppm_path}")

            try:
                from PIL import Image
                img = Image.frombytes("RGB", (w, h), bytes(rgb))
                png_path = "/tmp/avertm62_frame.png"
                img.save(png_path)
                print(f"Successfully saved PNG image to {png_path}")
            except Exception as e:
                print(f"PNG note: {e}")
        except Exception as e:
            print(f"PPM conversion note: {e}")
    else:
        print("No frames were dequeued within 10.0s. Check `dmesg | tail -n 40` for hardware status.")

if __name__ == "__main__":
    main()
