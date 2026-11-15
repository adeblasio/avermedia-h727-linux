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
| Composite (yellow) / S-Video | TM6202 internal decoder (AnalogSrc1 inputs 1 / 2) | Not implemented |
| RF antenna, analog | TDA18271 → TM6202 internal decoder (AnalogSrc1 input 0) | Not implemented |
| RF antenna, digital | TDA18271 → DRX-K → TM6202 MPEG-TS DMA | Not implemented |

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

Colour status after the DTO fix: the colour oscillator runs at exactly
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
- TS DMA route NOT found yet. Probed (with ABC locked, serial and parallel TS):
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
  - Windows' TS stream object (vtable 0x102570, ctor at 0x45920, base
    0x5ff7c) uses engine index 0 by default; the source index it writes to
    [19:18] comes from a field set outside the constructor (not found yet).

## Audio (INF)

- AnalogSrc1 audio: tuner = 2, composite = 0, S-Video = 0 (TAD internal decoder).
- AnalogSrc2 audio (HDMI/component side): WM8739 at 0x34 through the AD1871
  I2S mux. GPIO6 = I2S switch, GPIO2 = tuner audio, GPIO1 = composite audio.
- No ALSA capture device is implemented yet.

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
