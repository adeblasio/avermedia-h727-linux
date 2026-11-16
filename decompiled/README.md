# AVerMedia H727 (TM6202) Reverse-Engineering Reference

This folder contains reverse-engineered disassembly, register mappings, and hardware architectures extracted from the official Windows x64 driver (`AVerTM62_x64.sys`).

## Files in this Directory

Only `README.md` and `regmap.json` are in the repository. The Windows driver
binary and its disassembly are AVerMedia's copyright and are not
redistributed; extract the driver from AVerMedia's installer and disassemble
it yourself (for example with `objdump -d -M intel`) to follow the addresses
quoted here and in `HARDWARE.md`.

1. [`AVerTM62_x64.sys`](AVerTM62_x64.sys): Original Windows x64 PE driver binary (1.1 MB).
2. [`AVerTM62_x64.asm`](AVerTM62_x64.asm): Full disassembly of the entire `.text` section (x86_64 Intel syntax, 5.3 MB).
3. [`regmap.json`](regmap.json): Complete lookup table mapping driver internal object offsets to hardware BAR0 MMIO register offsets (`0x0000..0x0788`) and bitfield slices `[MSB:LSB]`.
4. [`tm6202_annotated_key_routines.asm`](tm6202_annotated_key_routines.asm): Curated disassembly of the core video, DMA, and interrupt routines with inline annotations mapping raw offsets to actual BAR0 registers.

---

## TM6202 Hardware Architecture & Routine Map

### 1. Dual DMA Engines & Channel Layout
The TM6202 chip contains two identical, independent DMA engines:
* **Engine 0** (`0x661ac` called with `r8 = 0x300`):
  * **Video DMA channel**: `0x0300` (FIFO `0x0500`, FIFO PTA `0x0504`, FIFO Count `0x0508`, FIFO Reset `0x050c`)
  * **VBI DMA channel**: `0x0330` (FIFO `0x0530`, FIFO PTA `0x0534`, FIFO Count `0x0538`)
  * **Audio DMA channel**: `0x0360` (FIFO `0x0560`)
  * **Engine 0 Controls**: IRQ Status `0x0390`, Format/Source routing `0x0394`, Timer `0x03a0`, Threshold `0x03a8`, Misc Clock `0x03b4`.
* **Engine 1** (`0x661ac` called with `r8 = 0x400`):
  * **Video DMA channel**: `0x0400` (FIFO `0x0600`, FIFO PTA `0x0604`, FIFO Count `0x0608`, FIFO Reset `0x060c`)
  * **VBI DMA channel**: `0x0430` (FIFO `0x0630`, FIFO PTA `0x0634`, FIFO Count `0x0638`)
  * **Audio DMA channel**: `0x0460` (FIFO `0x0660`)
  * **Engine 1 Controls**: IRQ Status `0x0490`, Format/Source routing `0x0494`, Timer `0x04a0`, Threshold `0x04a8`, Misc Clock `0x04b4`.

### 2. Key Routines in `tm6202_annotated_key_routines.asm`

| Function RVA | Name | Purpose |
|--------------|------|---------|
| `0x661ac` | `tm62_dma_engine_init` | Initializes an engine object (`0x300` or `0x400`) and sub-channels. |
| `0x666a0` | `tm62_dma_channel_init` | Maps channel slots (Status `+0x04..+0x14`, PTA `+0x18..+0x28`) and FIFOs. |
| `0x65cc4` | `tm62_stream_dma_channel_select` | Routes stream `ecx`: `1` = Video (`+0x00`), `2` = VBI (`+0x30`), `3` = Audio (`+0x60`). |
| `0x611c8` | `tm62_dma_poll_slots` | Reads the 5-slot completion history shift register for slots 0..4. |
| `0x645c0` | `tm62_dma_slot_event_processor`| Processes completed DMA descriptors and advances buffer queues. |
| `0x605cc` | `tm62_video_dma_format_cfg` | Configures line stride, video decoder source, and format filters. |
| `0x61498` | `tm62_dma_format_reg_writer` | Writes format configuration bitfields to `0x0394` / `0x0494`. |
| `0x653e8` | `tm62_dma_engine_start_stop`| Starts/stops the DMA frame timer (`0x07698000 / fps`). |
| `0x65fdc` | `tm62_dma_push_descriptor_fifo`| Writes page table PFN (`PTA >> 12`) and pushes descriptor `[31]=1` to FIFO. |
| `0x537fc` | `tm62_video_decoder_routing` | Programs HDMI vs Analog routing, timing modes, and SiI9011 clock gates. |
