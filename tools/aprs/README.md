# APRS on the UV-K5

Host-side tooling and test suite for the AX.25 and APRS layers in
`app/ax25.c` and `app/aprs.c`.

**State: a full two-way APRS station, on unmodified hardware.** Transmit is measured
and decoded by direwolf; receive is confirmed, including frames carrying no
alternating preamble - byte for byte what any other station sends.

## Running it

```sh
brew install direwolf       # provides atest and gen_packets
cd tools/aprs
make test                   # unit tests, bit clock, direwolf round trips, gate
make tones                  # just the gate
make demo                   # render a frame and decode it
```

And the firmware, which is a separate build from POCSAG:

```sh
make ENABLE_POCSAG=0 ENABLE_APRS=1 \
     APRS_CALL=SP9ABC APRS_SSID=7 APRS_GRID=JO90xa
```

`APRS_CALL` defaults to `NOCALL`, deliberately: measurements go into a dummy
load, and a real callsign in the default is an invitation to transmit one by
accident. Callsign, SSID, path and locator are all compiled in, because alpha
entry is where most of a full APRS screen's flash would go and none of it is
needed to get the transmitter measured.

`aprs_wav` renders a frame as AFSK audio with the tones on the command line,
which is the whole point of it:

```sh
./aprs_wav --space 2400 --snr 10 -o x.wav "SP9ABC-7>APZK5F,WIDE1-1:>hello"
atest x.wav
./aprs_wav --hex "SP9ABC-7>APZK5F:>hello"    # frame bytes, as atest -h prints them
./aprs_wav --air "SP9ABC-7>APZK5F:>hello"    # after stuffing and NRZI
```

## What the gate found

The BK4819's FFSK mode gives a 1200/2400 tone pair. Bell 202, which every TNC
and igate expects, is 1200/2200. `REG_72` sets the mark tone and the mode fixes
the ratio, so 2200 cannot be had without dragging the mark and the bit rate down
with it.

Rendering the same frame across a range of space tones, five trials each at 10 dB
SNR, decoded by stock direwolf 1.8.1:

| space | decoded | |
| --- | --- | --- |
| 2200 | 5/5 | Bell 202, the reference |
| 2250 | 5/5 | |
| 2300 | 5/5 | |
| 2350 | 5/5 | |
| 2375 | 5/5 | |
| **2400** | **4/5** | what the chip's FFSK 1200/2400 mode gives |
| 2425 | 0/5 | |
| 2450 | 0/5 | |
| 2500 | 0/5 | |
| 1800 | 0/5 | the chip's FFSK 1200/1800 mode |

So 2400 works, with about 25 Hz of margin before the cliff. And the margin
depends on which demodulator the receiver runs:

| demodulator | 2200 | 2400 |
| --- | --- | --- |
| `-P A` | 5/5 | 4/5 |
| `-P B` | 5/5 | 5/5 |
| `-P D` | 5/5 | 5/5 |
| `-P E` | 5/5 | 4/5 |

**Conclusion: FFSK 1200/2400 is good enough to prove the software chain but not
to rely on.** A transmit-only feature whose point is landing on aprs.fi through
arbitrary igates cannot depend on the far end running a tolerant demodulator, and
a chip tone even 25 Hz high would fail outright.

So the transmit path built here is exact Bell 202 in software instead:
`driver/bk4819-afsk.c` drives TONE1 alone with no FSK modem and rewrites `REG_71`
between **0x3065** (1200 Hz) and **0x58BA** (2200 Hz) at each bit boundary. One
SPI write per bit is nothing for a 48 MHz M0, and it escapes the modem's 256-byte
single-FIFO-load limit as well. `BK4819_PlayRogerNormal` already changes `REG_71`
mid-transmission, so the mechanism is not new to this hardware.

FFSK is **not** implemented. It would need `REG_58` made configurable in
`driver/bk4819-hwfsk.c`, which is POCSAG's shipped driver, and the gate says the
result would be less reliable than what is already here. Not worth the risk to
POCSAG for an inferior path.

## What the radio actually transmits

Measured 2026-09-20 with an RTL-SDR, one radio into a dummy load on 144.800:

| | measured | wanted |
| --- | --- | --- |
| mark | **1200.0 Hz**, spread 1.8 | 1200 |
| space | **2200.0 Hz**, spread 9.5 | 2200 |
| carrier | +550 Hz, which is 3.8 ppm and within the dongle's own error | 144.800 MHz |
| deviation | 2437 Hz on mark, 2201 on space, so about 2.3 to 2.6 kHz true | ~3 kHz |
| beacon length | 517 ms against 500 ms predicted | 1200 baud |

Both tones are exact. That is the measurement the whole bit banged path exists
for: the chip's FFSK mode would have given 2400, which the gate showed failing.

Deviation is a little low and differs between the tones, which suggests a low
pass somewhere in the chip's audio path attenuating 2200 more than 1200. Both
figures are inside what TNCs accept, and since it decodes there is no reason to
touch `DEV`.

**And it decodes.** A captured beacon put through stock direwolf:

```
[0] NOCALL-7>APZK5F,WIDE1-1,WIDE2-1:=5001.25N/01957.50E-UV-K5
```

on the default demodulator and on `-P B` and `-P D`. Tones, deviation, bit timing
and the AX.25 encoding all confirmed together, which no individual measurement can
do on its own.

## Measuring it yourself

The screen exists for this. Hold `0` on the main screen, then:

| row | |
| --- | --- |
| `FRQ` | frequency; `#` cycles 144.800, 144.390, 145.825 |
| `DEV` | `REG_70<14:8>`, the TONE1 gain, which for this path *is* the deviation |
| `TX` | POSITION, STATUS, TONE MARK, TONE SPACE, TONE 1010 |
| `SEC` | how long a tone test runs |

UP and DOWN move between rows, the side keys adjust, digits type, MENU transmits,
EXIT leaves. The bottom three rows show the source address, the path and the exact
payload that would go out.

Into a dummy load, with the RTL-SDR:

```sh
python3 ../pocsag/sdr_measure.py deviation --freq 144800000 --baud 1200
```

Use TONE MARK and TONE SPACE for the tone frequencies: they give a single steady
tone, which measures exactly. TONE 1010 reads the mark about 20 Hz high because
some half cycles straddle a tone change, but its change rate is the bit rate.

Then the end to end test. Set TX to POSITION, and capture the beacon rather than
piping `rtl_fm` live, because a saved burst can be re-examined without asking for
another transmission:

```sh
python3 sdr_afsk.py --window 2 --save beacon.iq --wav beacon.wav
atest beacon.wav
```

`--load beacon.iq` re-analyses a saved capture.

One trap this cost an hour on, in case the tool is ever rewritten. **Where there is
no carrier, the FM discriminator output is not quiet, it is enormous** —
differentiating the phase of noise gives excursions of hundreds of kHz. So the
analysis and the WAV both have to squelch on the IQ envelope, *and* take their DC
reference from inside the burst. Taking the median over a whole record where the
burst is only a quarter of it leaves the 50 kHz tuning offset in place, which
rails the WAV at full scale and makes a perfectly good frame undecodable. The
first measurement of this radio reported a carrier 84 kHz off and 28 kHz of
deviation for exactly that reason, on a signal that turned out to be flawless.

## Listening to yourself live

Decoding your own beacon off air, with no dependence on anyone else's igate, is
the test that separates "the firmware works" from "the signal is getting out".
Put an antenna on the dongle, a real antenna on the radio, and the radio in
another room so what is measured is a radio path rather than leakage across the
desk.

direwolf refuses to start without a config file even when it is only decoding, so:

```sh
cat > /tmp/direwolf.conf <<'EOF'
ADEVICE stdin null
ACHANNELS 1
CHANNEL 0
MYCALL N0CALL
MODEM 1200
AGWPORT 0
KISSPORT 0
EOF

rtl_fm -f 144.800M -M fm -s 24000 -l 0 -g 40 - \
  | direwolf -c /tmp/direwolf.conf -t 0 -n 1 -r 24000 -b 16 -
```

`-l 0` disables rtl_fm's squelch, which would otherwise chop the start of every
packet. `AGWPORT 0` and `KISSPORT 0` turn off the network listeners so this cannot
be mistaken for a real igate.

Two harmless complaints on the way past. PortAudio reports an "unexpected extreme
size of 0 bytes" audio buffer and recovers; stdin decoding works regardless,
which is worth checking once with a synthetic frame rather than wondering:

```sh
./aprs_wav --rate 24000 -o t.wav "M0TEST-7>APZK5F:>check" && tail -c +45 t.wav \
  | direwolf -c /tmp/direwolf.conf -t 0 -n 1 -r 24000 -b 16 -
```

And `UNKNOWN vendor/model` on every decode is the missing `tocalls.yaml`:
direwolf cannot look up which device `APZK5F` belongs to, because `APZ` is the
experimental prefix and this tocall is registered with nobody. Nothing to do
with the packet.

A good decode looks like this, which is the whole APRS layer and not just AX.25:

```
[0.2] NOCALL-7>APZK5F,WIDE1-1,WIDE2-1:=5001.25N/01957.50E-UV-K5
Position, House
N 50 01.2500, E 019 57.5000
UV-K5
```

The `audio level = 77(9/10)` line that precedes it is worth reading: the two
figures in brackets are the mark and space levels, and they came out within about
20% of each other, which is the same imbalance the deviation measurement found
independently.

## Receive, which works radio to radio

Two things about the chip stand in the way, and neither is settled from the
datasheet, because the POCSAG work caught this hardware contradicting its
register list twice.

**There is no Bell 202 demodulator.** The chip offers plain FSK, FFSK 1200/1800
and FFSK 1200/2400; APRS is 1200/2200.

It demodulates Bell 202 anyway, with the right mode and bandwidth. Getting there
took four wrong answers, all of them reasoned rather than measured, so the record is
worth keeping.

### What the probe found

Three rounds of changing one variable per flash-and-test got nowhere useful:

| transmitted | result | |
| --- | --- | --- |
| 1200/2200 | `S001 B0248 F00` | sync matched, no frame |
| 1200/2200 | `G00`, first bytes `23 91 c3 72 21 52` | 40% bit density: noise |
| 1200/2400 | `G01`, first bytes `01 7e db a9 85 fb` | first byte right, then noise |

So `RX` gained a `PROBE` mode that sweeps all six combinations of demodulator and
filter bandwidth against a known flag stream, scoring each on how many bytes of a
32 byte capture came back as `0x01`. One transmission per tone, and the answer was
unambiguous — scores out of 32:

| | 2400 Hz space | 2200 Hz space |
| --- | --- | --- |
| **narrow, FFSK1800** | **32** | **32** |
| narrow, FFSK2400 | 3 | 11 |
| wide, FFSK1800 | 0 | 5 |
| wide, FFSK2400 | 9 | 0 |
| either, FSK 1.2K | 0 | 1 |

**FFSK1800 with the narrow filter is perfect, and the space tone barely matters.**
Which disposes of the coherence argument above: every earlier failure was in
FFSK2400, which scores 3 to 11 whatever it is given. The variable was always the
demodulator and the bandwidth.

FFSK1800 reading a 2400 Hz space perfectly is the reverse of what the tone pairs
suggest, and no mechanism for it has been established, because none was measured.
The numbers are the finding, and they are encoded in `gModeNarrow` in
`app/aprs_rx.c`.

So the default is Bell 202 in both directions: one standards-compliant mode. The
`SPC` row that selects 2400 stays because it is how the comparison was made.

| `REG_58` | mode | |
| --- | --- | --- |
| `0x1CF3` | FFSK 1200/1800 | the measured winner, with the narrow filter |
| `0x10F9` | FFSK 1200/2400 | what the tone pair suggests, and it does not work |
| `0x00F1` | FSK 1.2K | what POCSAG receives on, kept as a control |

**The preamble detector will not hunt for a sync word** until it has seen whole
bytes of alternating bits, and a standards-compliant APRS transmission opens with
flags, whose NRZI form is `00000001` repeating. Our transmit path prepends
`AX25_LEAD_ALT` bytes of alternating bits for that reason, which costs nothing in
compatibility — the direwolf round trips run with it enabled and pass.

**But it turns out not to be needed.** A frame sent with no preamble at all, which
the `NO PREAMBLE` transmit option does, is received with a valid FCS. The FCS
validating is the informative part: it means the capture began before the address
field, so the sync was found on the flag stream rather than on the accidental
alternating run at the control/PID boundary. The detector had been armed before the
transmission started and **the sync hunt persists once armed** — channel noise
throws up an alternating byte roughly every 128 bit positions, so a quiet channel
arms it about nine times a second.

That is what makes real traffic receivable, and it was the open question from the
start.

Both are now sized for what the standards allow rather than what this radio
generates: `AX25_MAX_DIGIS` is 8 and `AX25_MAX_INFO` is 256, so no real frame is
refused for being too long. The receive capture length follows from
`AX25_MAX_AIR_BYTES` rather than being a separate number to keep in step.

That costs about three seconds of capture at 1200 baud, which is how long a
noise-armed capture leaves the receiver deaf in the worst case. In practice captures
end on the carrier drop or the stall timer rather than running to the programmed
length, which is what already happens with every frame.

### Reading the counters

The bottom two rows show `S` syncs, `B` bytes out of the FIFO, `F` frames whose
FCS passed, `X` captures examined, and `R` the raw RSSI at the last sync
(dBm = R/2 - 160). They matter more than they look:

| | means |
| --- | --- |
| `S` stays 0 | the preamble detector never armed, or the sync word never matched. The demodulator mode is the first thing to change |
| `S` climbs, `B` stays 0 | sync matched but no data followed, which points at the data length or the FIFO threshold |
| `S` and `B` climb, `F` stays 0 | the chain works and the bytes are wrong: demodulator mode, or bit timing |
| `F` climbs | done |

A sync count that moves while the frame count does not is a completely different
problem from nothing happening at all, which is the whole reason for showing both.

### Testing it

Both radios need this firmware: the transmitting one for the alternating
preamble, which older builds do not send, and the receiving one for the receive
path. It is the same image.

Put both on the same frequency, set `TX` to `POSITION` on one and press MENU, and
watch the other. Then cycle its `RX` row through the three modes and repeat. If
nothing syncs on any of them, the next thing to try is the filter bandwidth, which
is currently wide and could be narrow.

## The rate ceiling, and why it is 1200 baud

`REG_72`'s control word overflows above about 6347 baud, so the modem is not what
limits throughput. The radio's transmit audio path is. Deviation produced by a
steady tone, RTL-SDR into a dummy load:

| tone | deviation | relative |
| --- | --- | --- |
| 1200 Hz | 2437 Hz | 0 dB |
| 2200 Hz | 2201 Hz | −0.9 dB |
| 2400 Hz | 1205 Hz | **−6.1 dB** |
| 3600 Hz | 165 Hz | **−23.4 dB** |

A splatter filter with its corner just above 2200 Hz, doing exactly its job. Bell
202's space tone sits just inside it, which is luck rather than design.

Running the probe at 2400 baud with a 2400/3600 pair scored **zero in all six
combinations**, and these numbers say why: the space tone was 23 dB down, so it was
never transmitted. Nothing to do with the demodulator.

It also explains the slight tone dependence of the deviation at 1200 baud — 2200 Hz
is already on the filter's shoulder.

**So AFSK on this radio is capped at 1200 baud.** Anything faster has to abandon
audio tones for direct 2-level FSK, which shifts the carrier itself and bypasses the
filter entirely.

### Direct FSK: 4800 baud works

Measured K5 to K5, 64 of 64 bytes correct at both 2400 and 4800 — four times what
AFSK allows. The `TX` and `RX` rows both carry `FSK 2400` and `FSK 4800` options, and
the payload is a known repeating byte so the `G` counter scores it with no frame
involved.

| | |
| --- | --- |
| `REG_58` | `0x00F9` — FSK mode with the `<3:1>`=100 bandwidth the list gives "for FSK 2.4K" |
| `REG_72` | from the rate |
| sync word | 32 bits, POCSAG's, borrowed for its autocorrelation |
| `REG_59<10>` | set, because this chip inverts received data |

Two things cost a round each. The bandwidth field sat at the 1.2K setting at first:
2400 worked anyway with little margin, 4800 would not lock at all. And the register
list naming only "FSK 1.2K and FSK 2.4K" was taken as evidence 4800 could not work —
it is not, the naming does not constrain the demodulator.

`REG_72`'s word overflows above about 6347 baud, but **the FIFO bites first**: 16 bytes
with almost-full at 8, so 4800 leaves 13 ms before overflow against a 10 ms poll and
6000 leaves 10.7 ms. Faster means lowering the threshold so the interrupt fires
earlier, which is a constant POCSAG shares.

Direct FSK is not Bell 202 and nothing else will decode it — radio to radio only, with
APRS at 1200 baud for reaching the network.

## Register values

From `BK4819V3Registers_List_20201218.pdf`, not inferred:

| field | FFSK 1200/2400 |
| --- | --- |
| `REG_58<15:13>` TX mode | `011` |
| `REG_58<12:10>` RX mode | `100` (`111` = FFSK 1200/1800) |
| `REG_58<5:4>` preamble type | `11` = 0xAA |
| `REG_58<3:1>` RX bandwidth | `100` |

`REG_58` = **0x6039 TX, 0x7039 RX**, plus `0xC0` if the undocumented `<7:6>` is
set the way the aircopy and MDC code sets it.

`REG_72` is a *frequency*, not a baud rate: the list calls it "TONE2/FSK frequency
control word", freq(Hz) x 10.32444. The POCSAG driver's `BK4819_HwFskBaudWord`
only looks like a baud register because for FSK 1.2K the rate and the tone
coincide.

## Why the sync word is 0x01010101 and not 0x7E7E7E7E

NRZI-encode a flag and the pattern on air is not `0x7E` at all. `0x7E` is a bit
palindrome so transmission order does not matter; from level 1 the leading `0`
toggles, the six `1`s hold, the trailing `0` toggles back:

```
data  0 1 1 1 1 1 1 0
air   0 0 0 0 0 0 0 1      so a run of flags is 00000001 repeating
```

Every flag starts from level 1 again, so it is periodic, and the chip clocks sync
bytes most significant bit first, which POCSAG's `0x7CD215D8` working proves.
Matching `0x01010101` lands the FIFO exactly on an HDLC byte boundary with the
NRZI level known, so a capture is the frame from its first byte: addresses
included, FCS checkable. `TestFlagsAreZeroOneOnAir` guards the derivation.

Two things fall out of it. The data inversion POCSAG receive had to be told about
does not matter here, because NRZI codes transitions and a global inversion
cancels; `TestInversionDoesNotMatter` holds it. And sync polarity the chip
resolves itself through `REG_0B<7:6>`.

None of this helps until the preamble detector can be armed, which is the Phase 4
experiment. The chip wants whole bytes of `0xAA` or `0x55` before it will hunt for
a sync word, and an APRS transmission opens with flags.

## The golden vector

`TestAddressBytesMatchDirewolf` compares our address encoding against bytes taken
from direwolf 1.8.1 rather than from reading AX.25, because the SSID octet's C
bits are the one field where real APRS traffic and a literal reading of the
specification disagree. `gen_packets` for
`SP9ABC-7>APZK5F,WIDE1-1,WIDE2-1:>hello` gives SSID octets `e0 ee 62 63`: both
reserved bits set, `<7>` set on dest and source, clear on a digipeater that has
not repeated the frame.

`roundtrip.sh` goes further and compares whole frames byte for byte against
direwolf's own encoder, which currently matches exactly.

## Cost

Cross-compiled for Cortex-M0 at `-Os`:

| build | bytes | free of 60 kB |
| --- | --- | --- |
| neither feature | 53396 | 8044 |
| `ENABLE_POCSAG=1` | 59348 | 2092 |
| `ENABLE_APRS=1`, transmit only | 57572 | 3868 |
| `ENABLE_APRS=1`, both directions | 60556 | **884** |

So the whole feature costs **7160 bytes** of flash and 840 of RAM, and there is not
much room left. Alpha entry for callsigns and EEPROM persistence, the two things
still missing, would need some of it back — the probe and the `SPC` row are the
obvious candidates once the settings they found are no longer in question. A POCSAG build leaves 2092 bytes
free, which is less than the protocol layer alone, so **`ENABLE_APRS` is a
separate build**: the Makefile stops with an error if both are set. They share the
`0` key on the main screen.

The FCS is computed bitwise rather than from a table on the same grounds: 512
bytes of table against a frame a couple of hundred bytes long is a poor trade
when flash is the binding constraint.

## Tests

`ax25_test` is 151 checks in 16 groups. The ones that are load bearing rather
than routine:

| group | what would otherwise slip through |
| --- | --- |
| addresses match direwolf | the SSID C bits, guessed from the spec, going out wrong |
| flags are 0x01 on air | the NRZI derivation the receive sync word rests on |
| stuffing caps runs of ones | a frame that looks like a flag or an abort to the far end |
| inversion does not matter | needing a polarity setting on receive after all |
| single bit errors rejected | every one of ~1500 corruptions must fail the FCS |
| noise decodes nothing | 3000 random buffers producing invented text |
| truncated stream rejected | a dropped signal showing a partial message |
| non-UI frames rejected | connected-mode AX.25 being displayed as APRS |
| worst case fits one FIFO | a long frame silently needing a mid-packet refill |

`afsk_test` is a further 29 checks against a simulated SysTick, because bit
timing cannot be checked by reading it:

| group | what would otherwise slip through |
| --- | --- |
| feed across a wrap | one tick lost per 10 ms reload, invisible until a frame drifts |
| feed ignores a repeat | a repeated counter reading counted as a whole period |
| tone control words | the two frequencies this whole path exists to get exact |
| bit order is MSB first | a frame transmitted bit-reversed |
| repeated bits write nothing | a phase step on every bit, splattering the spectrum |
| no cumulative drift | 76 us of SPI per bit adding up to a 9% slow bit rate |
| transitions on boundaries | measured as spacing, which is what a demodulator sees |
| survives a slow poll | the SysTick interrupt stretching one bit in twelve |

`roundtrip.sh` and `tones.sh` are the external checks: they put the encoder
through a decoder nobody here wrote, which is the only kind of test that can
catch a whole layer being self-consistently wrong.

## Next

1. Measure the tones, the deviation and the bit rate, as above. Until that is
   done, nothing here is known to work on air.
2. Receive: the probe for the preamble detector, which is the POCSAG probe again
   and the one genuine unknown left. `AX25_FromAir` is already written and
   tested, so the question is purely whether the chip can be made to sync.
3. Alpha entry for the callsign, path and comment, and persisting them in EEPROM.
   Deferred because none of it is needed to measure the radio.
