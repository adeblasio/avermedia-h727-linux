# AVerMedia H727 driver for Linux

A Linux driver for the **AVerMedia H727** PCIe capture card (Trident/Entropic
**TM6202** bridge, PCI ID `1a0a:6202`). AVerMedia only ever shipped a Windows
driver, so this one was written by reverse-engineering that driver and probing
the hardware live.

| Input | Status |
|---|---|
| HDMI video (up to 1080i/1080p) | Working: V4L2 `/dev/videoN`, input 0 |
| HDMI audio | Working: ALSA card `H727` (48 kHz stereo) |
| Composite video (PAL) | Working: V4L2 input 1, colour, 25 fps |
| Digital TV (DVB-T) | Working: `/dev/dvb/adapterN` (frontend, demux, dvr) |
| Composite / analog audio | Not yet |
| Component (YPbPr), S-Video | Not tested |

It was developed against a card in Melbourne, Australia (PAL, DVB-T 7 MHz).
Other regions should work, but nobody has tested them yet.

## The card

| Chip | Job |
|---|---|
| TM6202 | PCIe bridge, DMA engines, analog video decoder (TM6010 core), audio block |
| Silicon Image SiI9011 | HDMI receiver |
| NXP TDA18271 | TV tuner (kernel `tda18271` driver) |
| Micronas DRX-3923K | DVB-T demodulator (kernel `drxk` driver) |
| Wolfson WM8739, ADI AD1871 | Analog audio ADCs |

[HARDWARE.md](HARDWARE.md) has the full register-level notes: what each block
does, the exact recipes that make video, TS and audio flow, and the dead ends.

## Building and loading

You need kernel headers and the usual build tools. The DVB part reuses the
kernel's own `drxk` and `tda18271` drivers; their private config headers are
included in `kheaders/`.

```bash
sudo apt install build-essential linux-headers-$(uname -r)
make
make load        # modprobes the dependencies and insmods avertm62.ko
```

Module parameters (all optional):

| Parameter | Default | Meaning |
|---|---|---|
| `dvb` | 1 | Register the DVB-T tuner, demux and dvr devices |
| `audio` | 1 | Register the ALSA capture card for HDMI audio |
| `drxk_parallel` | 0 | Parallel instead of serial TS from the demodulator |
| `static_ts_clk` | 0 | Fixed instead of dynamic TS clock |
| `debug_sysfs` | 0 | Expose DMA buffers in sysfs for experiments |

For development, `sudo sh install-devtools.sh` installs a root-owned reload
helper (`avertm62-load`) and the register tool `tm62reg` with a sudoers rule,
so reloading the module and poking registers need no password prompt.
`sudo sh install-devtools.sh --remove` undoes it.

## Watching

`viewer/h727view.py` is a small GTK viewer with an input switcher (HDMI,
composite, digital TV), deinterlacing, snapshots and a channel list:

```bash
sudo apt install gir1.2-gtk-3.0 gir1.2-gstreamer-1.0 gstreamer1.0-gtk3 \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav
python3 viewer/h727view.py                 # starts on the current V4L2 input
python3 viewer/h727view.py --tv "ABC TV"   # starts on a TV channel
```

HDMI and TV play with sound. Any V4L2/DVB/ALSA application should also work,
for example:

```bash
arecord -D hw:H727 -f S16_LE -c 2 -r 48000 hdmi.wav
```

### Digital TV

```bash
python3 tools/dvbt_scan.py                  # scan, saves ~/.config/h727/channels.json
python3 tools/dvbt_scan.py 177.5 184.5      # or scan given frequencies (MHz)
python3 tools/dvbt_record.py 219.5 --seconds 30 --out ten.ts
```

The scanner's built-in frequency list is Melbourne's. Pass your local
multiplex frequencies (and `--bw 8` outside Australia) to use it elsewhere.

## Known limitations

- HDMI audio only flows while HDMI video is streaming (the audio channel is
  clocked by the video DMA engine). The viewer always streams video, so this
  only matters for audio-only use.
- HDMI audio is captured at 48 kHz only.
- No audio yet for composite or the analog line inputs.
- Any write to the TM6202 GPIO registers resets the HDMI receiver; the driver
  reconfigures it when HDMI video starts.

## Credits

Written by [adeblasio](https://github.com/adeblasio), with reverse-engineering
and coding help from the AI assistants Claude (Anthropic) and Antigravity
(Google).

## Legal

This is an independent project, not affiliated with or endorsed by AVerMedia,
Trident/Entropic, Silicon Image or any other vendor. It contains no vendor code.
The register knowledge came from studying the hardware and AVerMedia's Windows
driver for interoperability. That driver and its disassembly are **not**
included.

Licensed under the GNU General Public License v2 or later (see [LICENSE](LICENSE)).
