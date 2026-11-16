#!/usr/bin/env python3
"""
h727view - live viewer for the AVerMedia H727 (avertm62 driver).

Shows the capture device in a window, with an input selector, a deinterlace
toggle, snapshots and a frame-rate readout. The "Digital TV" source plays
DVB-T channels (with sound) from the list saved by tools/dvbt_scan.py.
HDMI plays its audio from the driver's ALSA device (hw:H727); composite is
video only.

Usage: h727view.py [/dev/videoN] [--adapter N]
Needs GTK 3 and GStreamer (gir1.2-gtk-3.0, gir1.2-gstreamer-1.0,
gstreamer1.0-gtk3, gstreamer1.0-plugins-good; for TV also
gstreamer1.0-plugins-bad and gstreamer1.0-libav).
"""
import json
import ctypes
import fcntl
import os
import struct
import sys
import time

import gi
gi.require_version("Gtk", "3.0")
gi.require_version("Gst", "1.0")
gi.require_version("GstVideo", "1.0")
from gi.repository import GLib, Gst, GstVideo, Gtk  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from dvbt_tune import FE_READ_STATUS, dtv_properties, dtv_property, read_u, tune  # noqa: E402

CHANNELS_FILE = os.path.expanduser("~/.config/h727/channels.json")
TV_ID = "tv"
FE_GET_PROPERTY = 0x80106F53
DTV_STAT_CNR = 63
DMX_SET_PES_FILTER = 0x40146F2C
DMX_SET_BUFFER_SIZE = 0x6F2D
FE_HAS_LOCK = 0x10
VERBOSE = False
HDMI_ALSA = "hw:H727"
HDMI_ALSA_PROC = "/proc/asound/H727"
USE_GL = True
TV_DELAY = 800 * 1000000      # ns, see build_tv_pipeline
TV_STALL = 10                 # s without a video frame before restarting

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


def prefer_software_decoders():
    """Rank hardware video decoders (nv*dec, va*dec, ...) below libav.

    They wedge for good ("Couldn't find or determine first picture") after the
    corrupt packets a weak DVB-T signal produces, while avdec_* conceal the
    damage and carry on. A 1080i H.264 stream is easy work for the CPU."""
    for f in Gst.Registry.get().get_feature_list(Gst.ElementFactory):
        klass = f.get_metadata("klass") or ""
        if "Decoder" in klass and "Video" in klass and "Hardware" in klass:
            f.set_rank(Gst.Rank.NONE)


def load_channels():
    try:
        with open(CHANNELS_FILE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return []


class DvbTuner:
    """Keeps the frontend tuned and hands out a dvr0 fd carrying the whole mux."""

    def __init__(self, adapter):
        self.base = f"/dev/dvb/adapter{adapter}"
        self.fe = None
        self.freq = None
        self.dmx = None
        self.dvr = None

    def tune(self, freq_mhz, bw_mhz=7):
        if self.fe is None:
            self.fe = os.open(f"{self.base}/frontend0", os.O_RDWR | os.O_NONBLOCK)
        if freq_mhz != self.freq:
            tune(self.fe, freq_mhz, bw_mhz * 1000000)
            self.freq = freq_mhz

    def status(self):
        """(FE status bits, carrier-to-noise in dB or None)."""
        if self.fe is None:
            return 0, None
        st = read_u(self.fe, FE_READ_STATUS, "<I") or 0
        # the DRX-K's signal strength is always 100%; its CNR is meaningful
        prop = (dtv_property * 1)()
        prop[0].cmd = DTV_STAT_CNR
        cnr = None
        try:
            fcntl.ioctl(self.fe, FE_GET_PROPERTY, dtv_properties(1, prop))
            # struct dtv_fe_stats: u8 len; {u8 scale; s64 value}[]  (packed)
            if prop[0].data[0] and prop[0].data[1] == 1:      # FE_SCALE_DECIBEL
                cnr = struct.unpack_from("<q", bytes(prop[0].data), 2)[0] / 1000
        except OSError:
            pass
        return st, cnr

    def open_stream(self):
        """(Re)open demux0 with an all-PIDs filter; return a fresh dvr0 fd."""
        self.close_stream()
        self.dmx = os.open(f"{self.base}/demux0", os.O_RDWR)
        fcntl.ioctl(self.dmx, DMX_SET_BUFFER_SIZE, 8 * 1024 * 1024)
        # pid 0x2000 = all, input frontend, output TS tap (dvr0), PES other, start now
        fcntl.ioctl(self.dmx, DMX_SET_PES_FILTER,
                    struct.pack("<HxxIIII", 0x2000, 0, 2, 20, 4))
        self.dvr = os.open(f"{self.base}/dvr0", os.O_RDONLY)
        return self.dvr

    def close_stream(self):
        for fd in (self.dvr, self.dmx):
            if fd is not None:
                os.close(fd)
        self.dvr = self.dmx = None

    def close(self):
        self.close_stream()
        if self.fe is not None:
            os.close(self.fe)
        self.fe = None
        self.freq = None


class Viewer(Gtk.Window):
    def __init__(self, dev, adapter=0, start_channel=None):
        super().__init__(title="H727 Viewer")
        self.dev = dev
        self.tuner = DvbTuner(adapter)
        self.channels = load_channels()
        self.audio_linked = False
        self.radio = False
        self.started = self.last_frame_t = time.monotonic()
        self.gl_ok = bool(USE_GL and Gst.ElementFactory.find("gtkglsink") and
                          Gst.ElementFactory.find("glsinkbin"))
        self.video_linked = False
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
        if os.path.exists(f"{self.tuner.base}/frontend0"):
            self.input_combo.append(TV_ID, "Digital TV")
        try:
            self.input_combo.set_active_id(str(get_input(dev)))
        except OSError:
            self.input_combo.set_active(0)
        self.input_combo.connect("changed", self.on_input_changed)
        bar.pack_start(self.input_combo, False, False, 0)

        self.chan_combo = Gtk.ComboBoxText()
        for i, ch in enumerate(self.channels):
            label = ch["name"] + ("  (radio)" if ch.get("type") == "radio" else "")
            self.chan_combo.append(str(i), label)
        if self.channels:
            self.chan_combo.set_active(0)
        self.chan_handler = self.chan_combo.connect("changed", lambda *_: self.restart())
        self.chan_combo.set_no_show_all(True)
        bar.pack_start(self.chan_combo, False, False, 0)

        self.mute = Gtk.CheckButton(label="Mute")
        self.mute.connect("toggled", self.on_mute)
        self.mute.set_no_show_all(True)
        bar.pack_start(self.mute, False, False, 0)

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
        if start_channel:
            self.select_channel(start_channel)   # starts TV via the input switch
        if not self.pipeline:
            self.restart()

    # --- pipeline ---------------------------------------------------------
    def hdmi_audio_mode(self):
        """HDMI input selected and the driver registered its ALSA card."""
        name = self.input_combo.get_active_text() or ""
        return name.upper().startswith("HDMI") and os.path.exists(HDMI_ALSA_PROC)

    def tv_mode(self):
        return self.input_combo.get_active_id() == TV_ID

    def current_channel(self):
        idx = self.chan_combo.get_active_id()
        return self.channels[int(idx)] if idx is not None else None

    def build_pipeline(self):
        if self.tv_mode():
            return self.build_tv_pipeline()
        deint = "deinterlace mode=interlaced ! " if self.deint.get_active() else ""
        desc = (f"v4l2src device={self.dev} name=src ! video/x-raw,format=YUY2 ! "
                f"{deint}videoconvert ! gtksink name=sink sync=false")
        if self.hdmi_audio_mode():
            # the driver's ALSA device carries the HDMI audio (only while
            # HDMI video is streaming, which this pipeline does)
            desc += (f" alsasrc device={HDMI_ALSA} ! queue ! audioconvert ! "
                     "audioresample ! volume name=vol ! autoaudiosink")
        pipe = Gst.parse_launch(desc)
        vol = pipe.get_by_name("vol")
        if vol:
            vol.set_property("mute", self.mute.get_active())
        sink = pipe.get_by_name("sink")
        sink.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, self.count_frame)
        bus = pipe.get_bus()
        bus.add_signal_watch()
        bus.connect("message::error", self.on_error)
        return pipe, sink

    def build_tv_pipeline(self):
        ch = self.current_channel()
        if ch is None:
            raise RuntimeError(f"no channels - run tools/dvbt_scan.py (writes {CHANNELS_FILE})")
        self.tuner.tune(ch["freq_mhz"])
        fd = self.tuner.open_stream()
        # deinterlace mode=auto: only deinterlaces when the stream is flagged interlaced
        deint = "videoconvert ! deinterlace mode=auto ! " if self.deint.get_active() else ""
        # broadcasters send video up to a few seconds ahead of its audio, so the
        # per-stream queues must hold more than that or the demuxer blocks
        q = "max-size-time=10000000000 max-size-bytes=0 max-size-buffers=0"
        self.radio = ch.get("type") == "radio"
        sinkname = "glsinkbin" if self.gl_ok else "gtksink"
        # dvr0 delivers in real time but fdsrc is not a live source, so nothing
        # budgets for decode latency; render everything TV_DELAY late (both
        # sinks, to keep lip sync) so frames are not dropped as late.
        # HD needs GPU scaling/drawing (glsinkbin + gtkglsink): gtksink draws
        # through cairo and cannot keep up with 1080i at 50 fields/s.
        # Radio has no video stream, and an unfed video sink would hold the
        # pipeline in preroll forever, so radio gets no video branch at all.
        video = "" if self.radio else (
            f"queue name=vdec {q} ! decodebin ! {deint}videoconvert ! "
            f"{sinkname} name=sink ts-offset={TV_DELAY} ")
        desc = (f"fdsrc fd={fd} blocksize={188 * 348} ! "
                "queue max-size-buffers=0 max-size-time=0 max-size-bytes=16777216 ! "
                f"tsdemux name=demux program-number={ch['service_id']} "
                # one queue per stream: tsdemux pushes everything from a single
                # thread, so without them one prerolled sink starves the other
                f"{video}queue name=adec {q} ! decodebin ! audioconvert ! audioresample ! "
                f"volume name=vol ! autoaudiosink ts-offset={TV_DELAY}")
        pipe = Gst.parse_launch(desc)
        self.video_linked = self.audio_linked = False
        pipe.get_by_name("demux").connect("pad-added", self.on_demux_pad, pipe)
        pipe.get_by_name("vol").set_property("mute", self.mute.get_active())
        sink = pipe.get_by_name("sink")
        if sink:
            if self.gl_ok:
                sink.set_property("sink", Gst.ElementFactory.make("gtkglsink"))
            sink.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, self.count_frame)
        bus = pipe.get_bus()
        bus.add_signal_watch()
        bus.connect("message::error", self.on_error)
        return pipe, sink

    def on_demux_pad(self, _demux, pad, pipe):
        """Link the programme's first video and first audio stream to the decoders."""
        name = pad.get_name()
        if name.startswith("video") and not self.video_linked and not self.radio:
            target = pipe.get_by_name("vdec")
        elif name.startswith("audio") and not self.audio_linked:
            target = pipe.get_by_name("adec")
        else:
            return
        if pad.link(target.get_static_pad("sink")) == Gst.PadLinkReturn.OK:
            if name.startswith("video"):
                self.video_linked = True
            else:
                self.audio_linked = True

    def stop(self):
        if self.pipeline:
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline.get_state(Gst.CLOCK_TIME_NONE)
            for child in self.video_box.get_children():
                self.video_box.remove(child)
            self.pipeline = None
            self.sink = None
        self.tuner.close_stream()

    def restart(self):
        self.stop()
        tv = self.tv_mode()
        self.chan_combo.set_visible(tv)
        self.mute.set_visible(tv or self.hdmi_audio_mode())
        try:
            self.pipeline, self.sink = self.build_pipeline()
        except (GLib.Error, OSError, RuntimeError) as e:
            self.status.set_text(f"Error: {e}")
            print(f"pipeline error: {e}", file=sys.stderr)
            return
        if self.sink:
            inner = self.sink.get_property("sink") if self.sink.find_property("sink") else self.sink
            widget = inner.get_property("widget")
        else:
            ch = self.current_channel() or {}
            widget = Gtk.Label()
            widget.set_markup(f"<big><b>{GLib.markup_escape_text(ch.get('name', ''))}</b></big>\n(radio)")
        self.video_box.pack_start(widget, True, True, 0)
        widget.show()
        self.frames = 0
        self.last_t = self.started = self.last_frame_t = time.monotonic()
        self.pipeline.set_state(Gst.State.PLAYING)

    # --- callbacks ----------------------------------------------------------
    def count_frame(self, pad, info):
        self.frames += 1
        return Gst.PadProbeReturn.OK

    def update_fps(self):
        now = time.monotonic()
        fps = self.frames / (now - self.last_t) if now > self.last_t else 0
        if self.frames:
            self.last_frame_t = now
        elif (self.tv_mode() and self.pipeline and self.sink and
              now - self.last_frame_t > TV_STALL and now - self.started > TV_STALL):
            # reception dropouts can leave tsdemux/the decoder wedged; start over
            print("TV: no video for a while, restarting pipeline", file=sys.stderr)
            self.restart()
            return True
        self.frames, self.last_t = 0, now
        caps = ""
        if self.sink:
            pad = self.sink.get_static_pad("sink")
            c = pad.get_current_caps() if pad else None
            if c:
                s = c.get_structure(0)
                caps = f"{s.get_value('width')}x{s.get_value('height')}  "
        name = self.input_combo.get_active_text() or ""
        if self.tv_mode() and self.pipeline:
            ch = self.current_channel() or {}
            st, cnr = self.tuner.status()
            lock = "locked" if st & FE_HAS_LOCK else "NO LOCK"
            snr = f" SNR {cnr:.1f} dB" if cnr is not None else ""
            name = f"{ch.get('name', '')}  {ch.get('freq_mhz', 0):g} MHz {lock}{snr}"
        elif self.status.get_text().startswith("Error"):
            return True
        self.status.set_text(f"{name}  {caps}{fps:.1f} fps")
        if VERBOSE:
            pts = None
            if self.sink:
                smp = self.sink.get_property("last-sample")
                pts = smp.get_buffer().pts / 1e9 if smp else None
            lv = ""
            for qn in ("vdec", "adec"):
                qe = self.pipeline.get_by_name(qn) if self.pipeline else None
                if qe:
                    lv += f" {qn}={qe.get_property('current-level-buffers')}"
            print(self.status.get_text(), f"last pts {pts}{lv}", flush=True)
        return True

    def select_channel(self, name):
        """Switch to Digital TV on the channel called NAME (or list index NAME)."""
        for i, ch in enumerate(self.channels):
            if ch["name"].lower() == name.lower() or str(i) == name:
                self.chan_combo.handler_block(self.chan_handler)   # no restart yet
                self.chan_combo.set_active(i)
                self.chan_combo.handler_unblock(self.chan_handler)
                self.input_combo.set_active_id(TV_ID)
                return
        print(f"no channel called {name!r} in {CHANNELS_FILE}", file=sys.stderr)

    def on_mute(self, btn):
        vol = self.pipeline.get_by_name("vol") if self.pipeline else None
        if vol:
            vol.set_property("mute", btn.get_active())

    def on_input_changed(self, combo):
        idx = combo.get_active_id()
        if idx is None:
            return
        self.stop()
        if idx == TV_ID:
            self.restart()
            return
        self.tuner.close()
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
        self.tuner.close()
        Gtk.main_quit()


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("dev", nargs="?", default="/dev/video0")
    ap.add_argument("--adapter", type=int, default=0)
    ap.add_argument("--tv", metavar="CHANNEL", help="start on this TV channel (name or number)")
    ap.add_argument("--verbose", action="store_true", help="print the status line every second")
    ap.add_argument("--no-gl", action="store_true", help="draw TV with gtksink instead of OpenGL")
    a = ap.parse_args()
    global VERBOSE, USE_GL
    VERBOSE, USE_GL = a.verbose, not a.no_gl
    Gst.init(None)
    prefer_software_decoders()
    win = Viewer(a.dev, a.adapter, a.tv)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
