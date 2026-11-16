# AVerMedia H727 hardware map

Sources: the Windows INF (`h727_x64.inf`, BoardSetting/Decoder keys) and a
live I2C scan through the driver's adapters. I2C addresses are 8-bit.

## Chips

| Chip | Role | Bus / address | Notes |
|---|---|---|---|
| Trident TM6202 | PCIe bridge, DMA, internal analog video decoder ("VideoDecoder1"), internal audio decoder ("TAD") | PCI 1a0a:6202 | BAR0 32 KB |
| Silicon Image SiI9011 | HDMI receiver | I2C0 0x60 / 0x68 | Reset GPIO7, hot-plug GPIO3, interrupt GPIO0. **0x68:0x3e = 0xd7 enables the output bus, 0xd0 powers it down.** |
| ADI AD9985A | Component (YPbPr) analog front end | I2C0 0x98 | INF calls it the "Composite" decoder of AnalogSrc2 |
| NXP TDA18271 (C2, ID 0x84) | Silicon tuner, shared by analog and digital | I2C0 0xc0 | Mainline driver: `tda18271` |
| Micronas DRX-K | DVB-T/DVB-C demodulator | I2C0 0x52 (not seen in scan: held in reset) | Reset GPIO5. Mainline driver: `drxk`, needs firmware |
| Wolfson WM8739 | Stereo audio ADC (line-in) | I2C0 0x34 | Write-only I2C, so it doesn't show in a read scan |
| ADI AD1871 | I2S audio mux | none | Switched by GPIO6 |
| EEPROM | Board config | I2C0 0xa0 | Byte 0x30 = board ID |
| unknown | woken at D0 with 0x00=0x5a, 0x01=0x00, 0x00=0x00 | I2C1 0x80 | |

## Inputs (INF crossbar)

| Physical input | Path | Status in driver |
|---|---|---|
| HDMI | SiI9011 → TM6202 port 1 (0x0180 block) → DMA engine 1 | **Working**: 1080i60 at 30 fps |
| Component YPbPr | AD9985A → TM6202 | Not implemented |
| Composite (yellow) / S-Video | TM6202 internal decoder (AnalogSrc1 inputs 1 / 2) | **Working** (composite): PAL 720x576 colour, 25 fps |
| RF antenna, analog | TDA18271 → TM6202 internal decoder (AnalogSrc1 input 0) | Not implemented |
| RF antenna, digital | TDA18271 → DRX-K → serial TS → TM6202 DMA engine 0 | **Working**: DVB frontend, demux0, dvr0 |

## Composite path (TM6202 internal decoder), findings so far

The analog inputs come in through the card's separate breakout socket (it
looks like HDMI but carries the analog breakout cable), not the HDMI input.

Live-tested register settings that get the internal decoder producing video:

| Register | Value | Effect |
|---|---|---|
| `0x0290[27:26]` | 1 | Analog mux/clock into the internal decoder. With the driver's HDMI value (3) the decoder status never changes. 2 and 3 never lock. |
| `0x0000[0]` | 0 | Windows' routing (0x537fc) clears it for analog ports 0/1 |
| `0x0000[3:1]` | 4 | Only value that locks (weakly) |
| `0x0080[7:0]` | bit 0 set (0x15 default) | Lock lost with bit 0 clear |
| `0x0494[19:18]` | 0 | DMA engine 1 source = internal decoder (Windows thread 0x17f4c) |
| `0x0170` / `0x0174` | 0x02d00120 | Channel 0 crop 720x288 per field (PAL) |
| `0x0494[20]` | 1 | Hands every field to DMA separately. With 0 (interlaced) the engine drops every other field: 27 instead of 50 fields/s |

The driver now applies all of this when V4L2 input 1 ("Composite") is
selected, and weaves the fields into 720x576 frames at 25 fps.

Result: greyscale PAL video, one 720x288 field per DMA slot (414720 bytes,
about 50 fields/s, not woven by the hardware on this path). Still wrong:
no colour (colour burst `0x003c[0]` never sets, chroma bytes all 128), lock
quality only 0-1, picture offset horizontally.

Decoder picture and colour controls (confirmed by capture):

| Register | Meaning |
|---|---|
| `0x0008[7:0]` | Contrast (luma gain). Default 0x7e clips ~60% of a normal picture; 0x40 gives no clipping |
| `0x0008[15:8]` | Brightness (luma offset) |
| `0x0008[23:16]` | Saturation, but only scales V when colour is forced on |
| `0x0008[31:24]` | No visible effect so far |
| `0x000c[15]` | Forces colour on (colour-killer override). With it set, U is pinned at 240 and V offset negative: the demodulator never locks to the burst |
| `0x0004[28]` | Replaces the picture with a solid blue background |
| `0x0018` + `0x0020[31:24]` | Colour subcarrier word 0x2a098acb = exactly 4.43361875 MHz (PAL) at a 27 MHz decoder clock |

### The analog decoder is a Trident TM6010-family core

Byte n of BAR0 (0x00-0x8F) is the TM6010's video decoder register Rn
(REQ07 page in the old Linux tm6000 driver, removed after v5.x; see
`drivers/media/usb/tm6000/tm6000-regs.h` and `tm6000-stds.c` in v5.15).
Confirmed by: R04 LUMA_HAGC=0xdd, R17 HLOOP_MAXSTATE=0x8b, R31=0xc1 matching
tm6000's tables exactly, R1C-R1F HSYNC_DTO = 0x20000000 being tm6000's
0x1ccccccd rescaled from its 30 MHz clock to 27 MHz, and the R70-R7F status
registers (chroma DTO readback, AGC gains, chroma magnitude) behaving as named.

Useful registers (TM6010 names):

| BAR0 | Rn | Name | Notes |
|---|---|---|---|
| 0x0000[7:0] | R00 | VIDEO_CONTROL0 | [3:1] colour standard (tm6000: NTSC 0x01, PAL 0x33, PAL-M 0x05, PAL-N 0x37, SECAM 0x39), [5:4] 625-line. On the TM6202 bit 0 is not tm6000's: setting it removes the picture's colour |
| 0x0004[31:24] | R07 | OUTPUT_CONTROL | bit 4 = blue background |
| 0x0008 | R08-R0B | contrast, brightness, saturation, hue | |
| 0x000c | R0C-R0F | chroma AGC, CHROMA_KILL_LEVEL (R0D bit 7 forces colour on), auto position | |
| 0x0018 | R18-R1B | CHROMA_DTO_INCREMENT | **must be 0xcb8a092a** (R18 in the low byte) = 4.43361875 MHz at 27 MHz. The driver used to write 0x00cb8a09 + 0x2a in R23 (bytes shifted), putting the colour oscillator at ~1 MHz. Fixed. |
| 0x001c | R1C-R1F | HSYNC_DTO_INCREMENT | 0x20000000 at 27 MHz |
| 0x002c | R2C/R2D | CHROMA_BURST_START/END | 0x3a/0x4d (Windows) |
| 0x0038/0x003c | R38-R3F | VSYNC threshold, STATUS1-3 (R3A-R3C), R3F RESET (0x003c[24]) | Windows' table writes R38/R39/R3D fields the driver used to skip (now added) |
| 0x0070-0x007c | R70-R7F | HSYNC/CHROMA DTO readback, AGC analog/digital gain, chroma magnitude/gain, CORDIC freq, noise | live diagnostics |
| 0x0080 | R80-R83 | comb threshold/config, CHROMA_LOCK_CONFIG | |

**Composite colour works** (2026-11-16): two bugs, both from one bad
transcription of the Windows table:

1. `0x0018` (R18-R1B chroma DTO) had its bytes shifted; must be `0xcb8a092a`.
2. The shifted write also put `0x2a` into R23 HSYNC_PLL_END_TIME
   (`0x0020[31:24]`). Values 0x08-0x30 there stop the decoder seeing the
   colour burst; 0x00 and 0x38-0xf8 all lock. The driver now writes 0x80.
   The register survives module reloads, so it must be written explicitly.

With both fixed the decoder releases its colour killer by itself (R3A goes
0x04 -> 0x0e, burst bit set), hue is stable line to line, and Windows'
default contrast (0x7e) gives standard levels (white 236, black 20).

Historical notes from before the fix: the colour oscillator ran at exactly
4.4336 MHz and the decoder measures strong chroma (R7A ~0xee), but colour
stays killed. Forcing it on (R0D bit 7) gives correct, saturated colours in
alternating bands: the hue changes ~92 degrees per line (PAL's -90 degree
per-line subcarrier offset) i.e. the per-line burst phase correction never
happens, and R3A bit 4 (Windows' "signal present", 0x116cc) never sets.
Tried without success: every single-bit flip of R00-R03/R0C-R0F/R2C-R2F/
R30-R33/R80-R8B, a 2D burst-gate sweep, tm6000's R01-R03/R82/R83 values,
the 0x0294[19]/0x0290[20] bits, clearing the S-Video bits.

Colour-bar measurements (HDMI2AV adapter's own 8-bar test pattern; the
adapter is sharp and in full colour on a CRT):

- `0x029c[7:6]=3` (Windows decoder start, 0x11740) is required: without it
  the decoder never sees the input (flat grey picture, status frozen).
- Sample rate is correct: bar pitch ~90 px (13.5 MHz expects ~88).
- Horizontal detail is fine: bar edges rise 10-90% in ~3 px, and the two
  woven fields line up to within 0.1 px.
- The capture window starts ~137 px early (blanking on the left; the blue
  and black bars run off the right edge).
- The visible "blur" is cross-colour: inside a solid bar the luma swings
  about +/-90 levels in diagonal bands with a ~30 px period, i.e. the colour
  carrier is not being removed from luma (comb/notch not working, colour
  demodulator not locked).
- Do not bit-scan the clock block: flipping `0x0760[6]` hung the TM6202 so
  badly that a PCI bus reset could not recover it (reboot needed).

Earlier sharpness investigation (desktop content as source; the "2 MHz
low-pass" figure below turned out to be distorted by the cross-colour):

- `0x0000[3:1]=1` (Windows' value) removed a strong 7 px echo seen with 4
  (autocorrelation 0.39 -> 0.05) and unpinned the chroma (U/V centred on 128
  with colour forced on, but rainbow noise: no burst lock).
- The capture/DMA path is full resolution: the internal colour-bar generator
  gives single-pixel edges.
- The decoded composite luma rolls off smoothly: about -6 dB at 1.9 MHz,
  noise floor above 2.6 MHz, and no energy at the 4.43 MHz subcarrier in any
  mode. PAL composite should carry about 5 MHz.
- No effect on that bandwidth: single-bit flips of every decoder config bit
  (0x0004-0x0034, 0x0084-0x0134, 0x016c), the S-Video table values, the
  0x03c0/0x03d0 scalers, 0x0168, 0x0160, 0x0760[30:27], forcing colour on.
- Open question: whether the adapter's own output is band-limited (which would
  also remove the colour subcarrier) or the decoder's analog front end is.

Windows table selection: method 0x512a4 returns variant 0 only for S-Video
(crossbar input 2) and variant 1 for tuner and composite, so the v1 PAL table
(0x14678) is correct for composite. Windows routes composite as port 1
(`0x0290[27:26]=1`, `0x0000[0]=0`); the driver previously applied the port-2
(S-Video) routing at every stream start. `0x0000[3:1]` is not a strict input
select: Windows' value 1 also gives a picture.

Decoder status bits (from Windows 0x116cc / 0x11890, decoder object
constructor 0x67740): `0x0038[20]` signal, `0x0038[23:21]` lock quality,
`0x003c[0]` colour burst, `0x003c[1]` SECAM, `0x003c[2]` 50 Hz,
`0x003c[24]` decoder reset (1 = held).

Windows loads one decoder register table per video standard (lookup table at
0xfff50, dispatch at 0x10ea8). PAL B/G/D/H/I/G use 0x14678 (the table the
driver's `tm62_bus_master_init` copies); variant 0 (0x12698) differs only in
0x00c8, 0x00cc, 0x00d0, 0x00f8, 0x016c and audio-path bits. `0x0168` is a
per-standard PLL (Windows 0x10d60): 50 Hz = [7:3]=3, [23:19]=0x14, [15:8]=0x74.

The AD9985A is the **component** front end: its 17-register mode table
(regs at 0xb6538, values at 0xffec0) covers 480i/576i, 480p/576p, 720p and
1080i.

## Digital TV (DVB-T) findings

- DRX-K is held in reset by GPIO5 (driver drove it low); after releasing it
  the kernel's `drxk` driver finds a **DRX-3923K spin A3, 27 MHz crystal** and
  runs from its ROM code (no firmware file needed).
- TDA18271HD/C2 sits directly on I2C0 (answers with the DRX-K in reset), so
  `drxk_config.no_i2c_bridge = true`.
- With the driver's `dvb=1`, Melbourne Ten 219.5 MHz and ABC 226.5 MHz lock
  fully in 0.7 s; Seven/SBS partial, Nine nothing (antenna/frequency list).
### Transport stream (solved 2026-11-16)

The DRX-K drives a **serial** TS (with MVAL) into the TM6202, which delivers
it through **DMA engine 0**. Driver: `avertm62_ts_hw_start()`.

| Setting | Value | Source |
|---|---|---|
| DRX-K output | serial, MSTRT/MERR/MVAL/MCLK/MD0 enabled (kernel `enable_merr_cfg = true`) | Windows 0x219b8 drives every TS pin at 0x13. Without MVAL the TM6202 only takes ~13.5 Mbit/s |
| DRX-K clock | dynamic (kernel), or static at DTO period 2 (Windows: 32 Mbit/s, 0x217f0) | kernel static mode (50 Mbit/s, 50.6 MHz) also works |
| `0x019c[20]` | 1, TS input on | Windows 0x4fe64 |
| `0x019c[21]` | 1 = serial (INF "TS Stream Type" = 0, the default) | Windows 0x4fe64. The DRX-K is only set to parallel when that setting is non-zero (0x1cc79) |
| `0x0394` | `0x405a0138`: [15:0]=312 packets, [17:16]=2, [19:18]=2 (source "DigitalSrc1"), [20]=1, [22]=1, [30]=1 | Windows 0x5b69c writes [15:0], [19:18], [20]. **[22] is required**: without it engine 0 never takes a descriptor |
| `0x0390` | 0xe520 (312 x 188 bytes per descriptor) | Windows 0x5b69c |
| channel `0x0300` | stride 0, FIFO run, 10 ms, [24]=0, [31]=1 | Windows 0x65eec |
| IRQ | bit 0 (engine 0 slot done) fires once per descriptor | Windows enables IRQ 0x14 |

Windows' object model: the BDA TS filter (0x47700) sets source 2 or 3 from its
GUID ("DigitalSrc1"/"DigitalSrc2"); the pin (vtable 0x1025a0, type 4) opens a
capture unit (0x5c184). Sources 0/2/3 use unit 0 = engine 0 and source 1 uses
unit 1 = engine 1 (0x60e40). "DTV Source Type" (INF = 1) selects the DVB-T
demodulator object (0 ATSC, 1 DVB-T, 2 DVB-S, 3 DVB-C; 0x51448).

Things that turned out NOT to matter: `0x0160[7]`, `0x03a8`, the engine-0 timer
(`0x03a0`), `0x0190` bits, the TM6010-style digital-mode registers.

Results (Melbourne, antenna as installed): Ten 219.5 MHz runs at the full
23.05 Mbit/s (PCR-verified), no transport errors. ABC 226.5 MHz runs at full
rate but ~25% of packets carry the DRX-K's transport-error flag (reception).
Seven / SBS / Nine don't lock with this antenna. A DRX-K quirk: Ten's output
starts ~4 s after lock, ABC's immediately.

Tools: `tools/dvbt_record.py` (record / analyse a multiplex),
`tools/dvbt_scan.py` (channel list to `~/.config/h727/channels.json`).

### Earlier dead ends (kept for reference)

- Before the fix (MVAL off, engine 0 without [22]), probed with ABC locked:
  - `0x0160[7]=1` opens the capture gate Windows uses for this path
    (0x51f84); without it no channel receives any data.
  - Channel `0x330` (engine 0, second channel) = raw VBI samples from the
    analog decoder, 25920 bytes per transfer. Not TS.
  - Engine source `0x0394/0x0494[19:18]=0` = analog decoder video; 2 and 3
    never deliver data on channels 0x300/0x400/0x330/0x430/0x360/0x460.
  - Windows TS format (0x5b69c): `[19:18]=<source>`, `0x0490=0xe520`,
    `[15:0]=312` packets, `[20]=1`; route `0x019c[21:20]=3` ("TS Stream Type"
    defaults to 0). Applying these still delivers nothing on sources 2/3.
  - `0x07a0[6]=1` resets the DMA completion slots (Windows sets it when all
    streams are idle).
  - Windows configures the DRX-K TS output itself (Micronas reference code,
    0x1c9a4 init / 0x21c80 polarity / 0x217f0 DTO): all polarity inverts 0
    (same as the kernel driver), parallel mode chosen from the source's
    settings (INF "DTV Source Type" = 1, so almost certainly parallel).
  - Tested with ABC locked, parallel TS: sources 1/2/3 on channels
    0x300/0x400 deliver nothing even with the HDMI receiver's outputs
    powered down (`0x68:0x3e=0xd0`).
  - Toggling GPIO4, GPIO6 or GPIO7 makes the demodulator lose lock (GPIO7 is
    the INF's "SiI9011 reset" but evidently also disturbs the tuner side);
    a driver reload recovers it.
  - Overnight 2026-11-16, with ABC locked in both serial and parallel TS
    mode, nothing delivered TS data. Tried on channels 0x300/0x400/0x330/
    0x430/0x360/0x460 with engine sources 0-3:
    - TM6010 digital-mode settings: `RC0` (0x00c0[7:0]) bit 6 "TS input
      enable", `RCC` (0x00cc[7:0]) video/audio interface bits 5-6 cleared,
      `RFE` (0x00fc[23:16]) = 0x28
    - `0x0188[29]` and `[30]` (Windows 0x53b90 writes [29] from a per-device
      setting), with the HDMI receiver's outputs off
    - the second TS route pair `0x019c[17:16]` (Windows stream type 3)
    The 0x0730-0x0744 block is an audio clock (sample-rate dividers), not TS.
  - Windows' TS stream object (vtable 0x102570, ctor at 0x45920, base
    0x5ff7c) uses engine index 0 by default; the source index it writes to
    [19:18] comes from a field set outside the constructor (not found yet).

## Audio (INF)

- AnalogSrc1 audio: tuner = 2, composite = 0, S-Video = 0 (TAD internal decoder).
- AnalogSrc2 audio (HDMI/component side): WM8739 at 0x34 through the AD1871
  I2S mux. GPIO6 = I2S switch, GPIO2 = tuner audio, GPIO1 = composite audio.
### HDMI audio (solved 2026-11-16)

The driver registers an ALSA capture card `H727` (`hw:H727`, 48 kHz stereo
S16_LE). Recipe:

| Setting | Value | Notes |
|---|---|---|
| DMA channel | engine 1 audio channel `0x0460`, FIFO `0x0660`/`0x0664`, FIFO reset `0x066c[0]` | Windows stream type 3, channel object = engine object + 8 |
| Completion history | status `0x0464`.., PTA `0x0478`.., newest in slot 0 | same layout as the video/TS channels |
| Source | `0x0494[17:16]` = 1 | 0 = other I2S input (idle ADC, DC offset), 1 = SiI9011 I2S, 2 = none (video default), 3 = silence |
| Start | FIFO run `[30]`, 10 ms, channel `[24]`=0, `[31]`=1, push descriptors | generic channel start (Windows 0x65eec) |
| Timing | one descriptor closes per video frame (6400 bytes at 30 fps) | only while engine 1 runs, i.e. HDMI video must be streaming |

- The SiI9011 audio page needs nothing beyond the driver's init values; its
  outputs must be on (`0x68:0x3e = 0xd7`, done by HDMI video start).
- Proof it is the programme: left/right correlation 0.9, and the samples
  stop when the SiI9011 outputs are powered down.
- The level is low (about -45 dBFS RMS on the test source) but sounds right
  when played (user check), so it is the source level, not misalignment.
  SiI `0x68:0x26` bit 1 turns the samples into full-scale noise and bit 2
  into sign bits only, so the bit clock and data are genuinely arriving.

Pitfalls found on the way:

- Any write to the GPIO block (`0x0750`-`0x075c`), even rewriting a register
  with its current value, resets the SiI9011; it is reconfigured when HDMI
  video starts.
- The SiI9011's registers reset when the driver reloads and only come back
  when HDMI video starts.
- Engine 0's audio channel `0x0360` also runs (internally clocked, ~48 kHz)
  but carries only zeros on this board.
- `0x0280` is the internal audio block: `[3]` digital source select,
  `[7:4]` sample clock, `[8]` = 0 stops engine 0's audio channel,
  `[20:16]`/`[28:24]` volume, `[21]`/`[29]` mute. `0x0200`-`0x022c` is the
  TAD TV-sound decoder. `0x0700`-`0x071c` are the I2C controllers, `0x0730`
  -`0x0744` the IR receiver.
- `debug_sysfs=1` fills the debug DMA buffers with 0xaa and adds a ninth
  buffer that no stream uses, for experiments.

## GPIOs

| GPIO | Function |
|---|---|
| 0 | SiI9011 interrupt (input) |
| 1 | Audio select: composite / S-Video |
| 2 | Audio select: tuner |
| 3 | SiI9011 hot-plug |
| 5 | DRX-K reset |
| 6 | I2S switch (AD1871) |
| 7 | SiI9011 reset |
