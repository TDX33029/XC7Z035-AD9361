# TX BIT Engine v1.2

Vivado 2020.2 custom IP for the first IRIG 106 transmitter milestone.

## Data contract

- AXI4-Stream width: 32 bits, with `TKEEP` and `TLAST`.
- Byte lanes are consumed in the order lane 0, 1, 2, 3.
- Each valid byte is serialized MSB first.
- `BIT_COUNT` is authoritative, so the padding bits in the final byte are not emitted.
- One DMA transfer is one frame and must assert `TLAST` on its final beat.
- When `REPEAT_COUNT` is nonzero, the complete frame is captured once into a
  4096-word (16 KiB) PL block-RAM replay buffer. Finite repeats and
  `0xFFFFFFFF` infinite replay then run entirely in the AXIS clock domain;
  frame boundaries have no software/DMA gap. A zero repeat count retains the
  original streaming single-frame path and can use the larger DMA payload.
- `RATE_WORD` is the rounded Q0.32 ratio `BIT_RATE / AXIS_CLK_HZ`. It drives
  the only fractional scheduler in the BIT path, so the accumulator residue
  is preserved across symbol boundaries. `BIT_RATE` remains the literal Hz
  value used for validation and status.
- `BIT_PHASE` exports that same scheduler residue to the selected modulator.
  SOQPSK-TG therefore uses the exact BIT-engine symbol phase rather than an
  independently reset phase accumulator.
- Start only after the MM2S/FIFO path is primed. A gap after the first accepted beat is an underrun.

## Register map

| Offset | Name | Access | Meaning |
|---:|---|---|---|
| `0x00` | CTRL | W | bit0 START, bit1 STOP, bit2 CLEAR; write-one command |
| `0x04` | STATUS | R | bit0 BUSY, bit1 DONE, bit2 UNDERRUN, bit3 ERROR, bits15:8 ERROR_CODE |
| `0x08` | BIT_COUNT | R/W | Exact valid bit count |
| `0x0C` | REPEAT_COUNT | R/W | Additional PL replays; `0xFFFFFFFF` means infinite |
| `0x10` | PROFILE | R/W | Reserved waveform profile |
| `0x14` | CHANNEL | R/W | Reserved channel selection |
| `0x18` | BIT_RATE | R/W | Requested output bit rate, `1..AXIS_CLK_HZ` |
| `0x1C` | BITS_SENT | R | Accepted output bits in the current frame |
| `0x20` | FRAMES_COMPLETED | R | Completed frame count in current job |
| `0x24` | ERROR_CODE | R | Error code in bits7:0 |
| `0x28` | VERSION | R | `0x00010200` |
| `0x2C` | RATE_WORD | R/W | Rounded Q0.32 value `BIT_RATE / AXIS_CLK_HZ` |

Error codes: `01` zero count, `02` zero TKEEP, `03` early TLAST,
`04` missing TLAST, `05` stream gap/underrun, `06` zero bit rate,
`07` bit rate above `AXIS_CLK_HZ`.

## Vivado packaging

From the AD9361TX workspace root:

```tcl
vivado -mode batch -source repo/tx_bit_engine/package_ip.tcl
```

Then refresh the IP repository and upgrade the BD instance to **TX BIT Engine v1.2**.

Set `AXIS_CLK_HZ` to the real frequency connected to `axis_clk`. For the
current 61.44 MSPS LVDS design this is 61.44 MHz. Connect `bit_ready` to the
selected modulator's input-ready signal, and connect `bit_phase` to the
SOQPSK-TG modulator phase input. Repeated frames must be no larger
than 16 KiB; the PS and UI reject larger repeated jobs before DMA submission.
