#!/usr/bin/env python3
"""
h727view - live viewer for the AVerMedia H727 (avertm62 driver).

Shows the capture device in a window, with an input selector, a deinterlace
toggle, snapshots and a frame-rate readout. Video only: the driver has no
audio capture yet.

Usage: h727view.py [/dev/videoN]
Needs GTK 3 and GStreamer (gir1.2-gtk-3.0, gir1.2-gstreamer-1.0,
gstreamer1.0-gtk3, gstreamer1.0-plugins-good).
"""
import ctypes
import fcntl
import os
import sys
import time

import gi
gi.require_version("Gtk", "3.0")
gi.require_version("Gst", "1.0")
gi.require_version("GstVideo", "1.0")
from gi.repository import GLib, Gst, GstVideo, Gtk  # noqa: E402

VIDIOC_ENUMINPUT = 0xc050561a
VIDIOC_G_INPUT = 0x80045626
VIDIOC_S_INPUT = 0xc0045627
V4L2_IN_ST_NO_SIGNAL = 0x00000002


class v4l2_input(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("name", ctypes.c_char * 32),
        ("type", ctypes.c_uint32),
        ("audioset", ctypes.c_uint32),
        ("tuner", ctypes.c_uint32),
        ("std", ctypes.c_uint64),
        ("status", ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 3),
    ]


def list_inputs(dev):
    """Return [(index, name, has_signal)] from VIDIOC_ENUMINPUT."""
    out = []
    fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
    try:
        i = 0
        while True:
            inp = v4l2_input(index=i)
            try:
                fcntl.ioctl(fd, VIDIOC_ENUMINPUT, inp)
            except OSError:
                break
            out.append((i, inp.name.decode(errors="replace"),
                        not (inp.status & V4L2_IN_ST_NO_SIGNAL)))
            i += 1
    finally:
        os.close(fd)
    return out


def get_input(dev):
    fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
    try:
        v = ctypes.c_int(0)
        fcntl.ioctl(fd, VIDIOC_G_INPUT, v)
        return v.value
    finally:
        os.close(fd)


def set_input(dev, index):
    fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, VIDIOC_S_INPUT, ctypes.c_int(index))
    finally:
        os.close(fd)


class Viewer(Gtk.Window):
    def __init__(self, dev):
        super().__init__(title="H727 Viewer")
        self.dev = dev
        self.pipeline = None
        self.sink = None
        self.frames = 0
        self.last_t = time.monotonic()
        self.set_default_size(1024, 640)
        self.connect("destroy", self.on_quit)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.add(box)

        bar = Gtk.Box(spacing=8, margin=6)
        box.pack_start(bar, False, False, 0)

        bar.pack_start(Gtk.Label(label="Input:"), False, False, 0)
        self.input_combo = Gtk.ComboBoxText()
        for idx, name, _ in list_inputs(dev):
            self.input_combo.append(str(idx), name)
        try:
            self.input_combo.set_active_id(str(get_input(dev)))
        except OSError:
            self.input_combo.set_active(0)
        self.input_combo.connect("changed", self.on_input_changed)
        bar.pack_start(self.input_combo, False, False, 0)

        self.deint = Gtk.CheckButton(label="Deinterlace")
        self.deint.set_active(True)
        self.deint.connect("toggled", lambda *_: self.restart())
        bar.pack_start(self.deint, False, False, 0)

        snap = Gtk.Button(label="Snapshot")
        snap.connect("clicked", self.on_snapshot)
        bar.pack_start(snap, False, False, 0)

        self.status = Gtk.Label(label="", xalign=1.0)
        bar.pack_end(self.status, True, True, 0)

        self.video_box = Gtk.Box()
        box.pack_start(self.video_box, True, True, 0)

        GLib.timeout_add(1000, self.update_fps)
        self.restart()

    # --- pipeline ---------------------------------------------------------
    def build_pipeline(self):
        deint = "deinterlace mode=interlaced ! " if self.deint.get_active() else ""
        desc = (f"v4l2src device={self.dev} name=src ! video/x-raw,format=YUY2 ! "
                f"{deint}videoconvert ! gtksink name=sink sync=false")
        pipe = Gst.parse_launch(desc)
        sink = pipe.get_by_name("sink")
        sink.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, self.count_frame)
        bus = pipe.get_bus()
        bus.add_signal_watch()
        bus.connect("message::error", self.on_error)
        return pipe, sink

    def stop(self):
        if self.pipeline:
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline.get_state(Gst.CLOCK_TIME_NONE)
            for child in self.video_box.get_children():
                self.video_box.remove(child)
            self.pipeline = None
            self.sink = None

    def restart(self):
        self.stop()
        self.pipeline, self.sink = self.build_pipeline()
        widget = self.sink.get_property("widget")
        self.video_box.pack_start(widget, True, True, 0)
        widget.show()
        self.frames = 0
        self.last_t = time.monotonic()
        self.pipeline.set_state(Gst.State.PLAYING)

    # --- callbacks ----------------------------------------------------------
    def count_frame(self, pad, info):
        self.frames += 1
        return Gst.PadProbeReturn.OK

    def update_fps(self):
        now = time.monotonic()
        fps = self.frames / (now - self.last_t) if now > self.last_t else 0
        self.frames, self.last_t = 0, now
        caps = ""
        if self.sink:
            pad = self.sink.get_static_pad("sink")
            c = pad.get_current_caps() if pad else None
            if c:
                s = c.get_structure(0)
                caps = f"{s.get_value('width')}x{s.get_value('height')}  "
        name = self.input_combo.get_active_text() or ""
        self.status.set_text(f"{name}  {caps}{fps:.1f} fps")
        return True

    def on_input_changed(self, combo):
        idx = combo.get_active_id()
        if idx is None:
            return
        self.stop()
        try:
            set_input(self.dev, int(idx))
        except OSError as e:
            self.status.set_text(f"Input switch failed: {e}")
            return
        self.restart()

    def on_snapshot(self, _btn):
        sample = self.sink.get_property("last-sample") if self.sink else None
        if not sample:
            self.status.set_text("No frame yet")
            return
        png = GstVideo.video_convert_sample(sample, Gst.Caps.from_string("image/png"),
                                            Gst.SECOND)
        buf = png.get_buffer()
        ok, info = buf.map(Gst.MapFlags.READ)
        if not ok:
            return
        path = os.path.expanduser(time.strftime("~/h727_%Y%m%d_%H%M%S.png"))
        with open(path, "wb") as f:
            f.write(info.data)
        buf.unmap(info)
        self.status.set_text(f"Saved {path}")

    def on_error(self, _bus, msg):
        err, dbg = msg.parse_error()
        self.status.set_text(f"Error: {err.message}")
        print(f"GStreamer error: {err.message}\n{dbg}", file=sys.stderr)

    def on_quit(self, *_):
        self.stop()
        Gtk.main_quit()


def main():
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/video0"
    Gst.init(None)
    win = Viewer(dev)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
