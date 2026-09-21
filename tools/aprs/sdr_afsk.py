#!/usr/bin/env python3
"""Measure the UV-K5's Bell 202 AFSK with an RTL-SDR.

    ./sdr_afsk.py                 # wait for a burst, then report
    ./sdr_afsk.py --seconds 4     # fixed window instead of waiting

Answers the questions tools/aprs/tones.sh could not: what tones the radio
actually emits, how much FM deviation they produce, and whether a bit really
lasts 833.33us. The gate found a 2425Hz space failing to decode where 2400
passed, so an error of a couple of hundred Hz is the difference between working
and not.

Why not tools/pocsag/sdr_measure.py: that was written for 2-level FSK, where the
instantaneous frequency sits at one of two levels and the spectrum shows two
tones 2*deviation apart. AFSK is different in two ways that make its numbers
misleading here. The discriminator output is a sine at the audio frequency, so a
median of its upper and lower halves reads 0.707 of the peak deviation rather
than the peak. And the RF spectrum is an FM signal with Bessel sidebands spaced
at the audio frequency, so the "two tones" it finds are sidebands, not the mark
and space at all.

What this measures instead, straight from the FM discriminator:

  tone            the audio frequency, from interpolated zero crossings
  deviation       the amplitude of the discriminator sine, band limited so the
                  RTL-SDR's 8-bit noise does not inflate it
  change rate     tone changes per second, which in the 1010 pattern is the
                  bit rate

Set the screen's TX row to TONE MARK, TONE SPACE or TONE 1010 and transmit into a
dummy load.

Use the steady tone tests for the tone frequencies: against synthetic signals they
come out exact, where the 1010 pattern reads the mark about 20Hz high because
some half cycles straddle a tone change. Use 1010 for the change rate.

Checked against synthetic AFSK of known tone, deviation and rate, including the
cases that should fail: a 2400Hz space, half the wanted deviation, and tones 8%
high as a wrong crystal would give.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

import numpy as np

SAMPLE_RATE = 1_024_000
OFFSET_HZ   = 50_000       # tune below the signal, clear of the DC spike

MARK_HZ     = 1200.0
SPACE_HZ    = 2200.0
BAUD        = 1200.0

# Overridden by --mark/--space/--baud, because the radio can now be set to 2400
# baud with a 2400/3600 pair and the tone classifier has to know which pair to
# expect. Module level so the analysis functions see the change.
WANT_DEV_HZ = 3000.0       # narrow band FM voice deviation, what APRS uses

# Halfway between the tones, in the log domain so it sits sensibly between them.
TONE_SPLIT  = (MARK_HZ * SPACE_HZ) ** 0.5


def set_tones(mark, space, baud):
    """Point the analysis at a different tone pair and rate."""
    global MARK_HZ, SPACE_HZ, BAUD, TONE_SPLIT

    MARK_HZ, SPACE_HZ, BAUD = float(mark), float(space), float(baud)
    TONE_SPLIT = (MARK_HZ * SPACE_HZ) ** 0.5


def capture(seconds, gain, tune_hz, device=0):
    n    = int(SAMPLE_RATE * seconds)
    path = tempfile.mktemp(suffix=".iq")

    cmd = ["rtl_sdr", "-f", str(tune_hz), "-s", str(SAMPLE_RATE),
           "-n", str(n), "-d", str(device), "-g", str(gain), path]

    for attempt in range(3):
        r = subprocess.run(cmd, capture_output=True, text=True)

        if r.returncode == 0 and os.path.exists(path):
            break

        # "No supported devices found" right after a previous capture is usually
        # the USB device not yet released rather than a real absence.
        if "No supported devices" in (r.stderr or "") and attempt < 2:
            print("  device busy, settling then retrying...")
            time.sleep(2)
            continue

        print(r.stderr[-400:], file=sys.stderr)
        raise SystemExit("rtl_sdr failed")

    raw = np.fromfile(path, dtype=np.uint8)
    os.unlink(path)

    iq = ((raw[0::2].astype(np.float32) - 127.5) +
          1j * (raw[1::2].astype(np.float32) - 127.5))

    return iq


def strongest_window(iq, win):
    power = np.abs(iq) ** 2

    if len(power) <= win:
        return iq

    step = win // 8
    best, best_p = 0, -1.0

    for start in range(0, len(power) - win, step):
        p = power[start:start + win].mean()

        if p > best_p:
            best, best_p = start, p

    return iq[best:best + win]


def wait_for_burst(gain, tune_hz, device, chunk=4.0, minutes=5.0, margin_db=8.0,
                   window=1.0):
    print("learning the noise floor...")
    floor = float(np.abs(capture(1.5, gain, tune_hz, device)).mean())
    need  = floor * (10 ** (margin_db / 20))

    print(f"  floor RMS {floor:.2f}, will trigger above {need:.2f}")
    print(f"\nwaiting up to {minutes:.0f} min. Press MENU on the radio now.\n")

    deadline = time.time() + minutes * 60
    n = 0

    while time.time() < deadline:
        n += 1
        iq  = capture(chunk, gain, tune_hz, device)
        win = strongest_window(iq, int(SAMPLE_RATE * window))
        rms = float(np.abs(win).mean())

        print(f"  chunk {n}: peak RMS {rms:6.2f} "
              f"({20 * np.log10(max(rms, 1e-9) / max(floor, 1e-9)):+.1f} dB)"
              + ("   <- BURST" if rms > need else ""))

        if rms > need:
            return win

    print("\nnothing detected. Is the radio transmitting on this frequency?")
    return None


def discriminate(iq):
    """Instantaneous frequency in Hz, which for FM is the modulating audio."""
    return np.diff(np.unwrap(np.angle(iq))) * SAMPLE_RATE / (2 * np.pi)


# The discriminator is low pass filtered and decimated before anything looks at
# it. Two reasons: differentiating phase amplifies noise enormously on an 8-bit
# RTL-SDR, and at 1.024MHz an FFT of anything as short as one bit has bins
# thousands of Hz apart, which is useless for telling 2200 from 2400.
FS2   = 64_000
DECIM = SAMPLE_RATE // FS2


def lowpass_decimate(x, cutoff_hz=9000.0, taps=257):
    """Windowed sinc low pass, then take every DECIM'th sample.

    The cutoff is well above both tones so neither is attenuated and the
    deviation can still be measured off the result, but far below the noise
    bandwidth, which it cuts by about an order of magnitude.
    """
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff_hz / SAMPLE_RATE * n) * np.hanning(taps)
    h /= h.sum()

    return np.convolve(x, h, mode="same")[::DECIM]


# 1.024MHz divides by 32 to give a standard audio rate, so a WAV written for
# direwolf needs no resampling.
WAV_RATE = 32_000


def open_region(iq, open_at=0.25, pad_s=0.002):
    """The part of a capture that actually has a carrier in it.

    Squelch is not a nicety here. With no carrier the discriminator output is not
    quiet, it is enormous: differentiating the phase of noise gives excursions of
    hundreds of kHz, which swamp a few kHz of real deviation. Everything
    downstream has to look only at the open part, and has to take its DC
    reference from there as well. Getting that wrong is what made the first
    measurement of this radio report a carrier 84kHz off and a deviation of
    28kHz, on a signal that was in fact perfect.

    A fraction of the peak rather than a multiple of the noise floor, so this
    behaves whether the burst fills the capture or is a small part of it. Returns
    the discriminator output, and the slice of it holding the longest burst.
    """
    inst = discriminate(iq)
    mag  = np.abs(iq[:len(inst)])

    mask = mag > (mag.max() * open_at)

    # Widen a little so the first and last bits are not clipped.
    pad  = int(SAMPLE_RATE * pad_s)
    mask = np.convolve(mask, np.ones(2 * pad + 1), mode="same") > 0

    # The longest contiguous run, rather than every sample above the threshold:
    # boolean indexing would splice separate bursts together and invent a
    # transition at the join.
    edges = np.flatnonzero(np.diff(np.concatenate(([False], mask, [False]))))
    starts, ends = edges[0::2], edges[1::2]

    if len(starts) == 0:
        return inst, slice(0, len(inst))

    k = int(np.argmax(ends - starts))

    return inst, slice(int(starts[k]), int(ends[k]))


def write_wav(iq, path, peak_dev=3000.0):
    """The FM discriminator output as a WAV, which is what a TNC wants to hear.

    This is the end to end test: tones and deviation measured separately can both
    look right while the bit timing is wrong, and only a decoder settles it.
    Scaled so the nominal deviation is most of full scale, since direwolf
    normalises level anyway.

    Squelched on the IQ envelope, which is not optional. With no carrier the
    discriminator output is not quiet, it is enormous: differentiating the phase
    of noise gives excursions of hundreds of kHz. Written unsquelched, a capture
    holding a 0.5s burst in 2s of noise comes out as railed noise with the frame
    buried below it, and direwolf refuses it. Squelched, the same capture decodes.
    """
    inst, burst = open_region(iq)

    # The DC reference comes from inside the burst. Taking it over the whole
    # record leaves the tuning offset in place, which after scaling rails the
    # output at full scale and makes the frame undecodable.
    quiet = np.zeros_like(inst)
    quiet[burst] = inst[burst] - np.median(inst[burst])

    audio = lowpass_decimate(quiet, cutoff_hz=9000.0)[::FS2 // WAV_RATE]

    pcm = np.clip(audio / peak_dev * 20000.0, -32768, 32767).astype(np.int16)

    with open(path, "wb") as f:
        n = len(pcm) * 2

        f.write(b"RIFF")
        f.write((36 + n).to_bytes(4, "little"))
        f.write(b"WAVEfmt ")
        f.write((16).to_bytes(4, "little"))
        f.write((1).to_bytes(2, "little"))            # PCM
        f.write((1).to_bytes(2, "little"))            # mono
        f.write(WAV_RATE.to_bytes(4, "little"))
        f.write((WAV_RATE * 2).to_bytes(4, "little"))
        f.write((2).to_bytes(2, "little"))
        f.write((16).to_bytes(2, "little"))
        f.write(b"data")
        f.write(n.to_bytes(4, "little"))
        f.write(pcm.tobytes())

    return len(pcm) / WAV_RATE


def deviation(x, lo=600.0, hi=5000.0):
    """Peak FM deviation, as the amplitude of the discriminator sine.

    Both tones are generated at the same gain, so the RMS over a whole burst is
    the same whichever one is playing and there is no need to split it up.
    Keeping only the audio band first stops the remaining noise inflating it:
    for a sine, peak is root two times RMS.
    """
    n = len(x)
    X = np.fft.rfft(x - x.mean())
    f = np.fft.rfftfreq(n, 1 / FS2)

    X[(f < lo) | (f > hi)] = 0

    return float(np.sqrt(2.0) * np.std(np.fft.irfft(X, n)))


def half_cycles(x):
    """Frequency of every half cycle, from linearly interpolated zero crossings.

    Interpolating rather than taking the sample index matters: at 64kHz a half
    cycle of 2200Hz is only 14.5 samples, so rounding to whole samples would
    quantise the answer to about 7%. Interpolated and then taken as a median over
    hundreds of half cycles, this resolves single Hz.
    """
    y = x - np.median(x)

    sign  = np.signbit(y)
    idx   = np.flatnonzero(np.diff(sign))

    if len(idx) < 4:
        return np.array([]), np.array([])

    # linear interpolation of where y actually crosses zero
    a, b  = y[idx], y[idx + 1]
    exact = idx + np.where(b != a, a / (a - b), 0.5)

    gaps  = np.diff(exact) / FS2

    with np.errstate(divide="ignore", invalid="ignore"):
        freqs = 1.0 / (2.0 * gaps)

    good = np.isfinite(freqs) & (freqs > 300.0) & (freqs < 8000.0)

    return exact[:-1][good], freqs[good]


def verdict(got, want, tol, unit="Hz"):
    off = got - want
    ok  = abs(off) <= tol

    return (f"{got:8.1f} {unit}  want {want:.1f}, off {off:+.1f} "
            f"({'ok' if ok else 'OUT OF TOLERANCE'})")


def report(iq, tune_hz):
    inst, burst = open_region(iq)

    held = inst[burst]

    print(f"\nburst      {len(held) / SAMPLE_RATE * 1000:.0f} ms of the "
          f"{len(inst) / SAMPLE_RATE * 1000:.0f} ms captured")

    # Trim the edges: keying transients at both ends would drag every statistic.
    trim  = max(1, len(held) // 20)
    audio = lowpass_decimate(held[trim:-trim])

    centre = float(np.median(audio))

    print(f"\ncarrier    {tune_hz + centre:,.0f} Hz "
          f"({centre - OFFSET_HZ:+,.0f} Hz from where it was told to be)")

    dev = deviation(audio)

    print(f"deviation  {verdict(dev, WANT_DEV_HZ, 900.0)}")
    print("           peak, over the whole burst. The DEV row on the radio moves")
    print("           it. Reads about 6% low against a synthetic signal of known")
    print("           deviation, because band limiting trims the skirts, so treat")
    print("           it as a floor rather than an exact figure.")

    at, freqs = half_cycles(audio)

    if len(freqs) < 8:
        print("\ncould not find a tone. Too weak, or not transmitting?")
        return

    is_space = freqs > TONE_SPLIT

    print(f"\n{len(freqs)} half cycles measured")

    for flag, name, want in ((False, "mark ", MARK_HZ), (True, "space", SPACE_HZ)):
        group = freqs[is_space == flag]

        # A handful either side of the split are half cycles that straddle a tone
        # change, so ignore a class that is only those.
        if len(group) < max(8, len(freqs) // 50):
            continue

        print(f"  {name}    {verdict(float(np.median(group)), want, 50.0)}")
        print(f"             {len(group)} half cycles, "
              f"spread {float(np.std(group)):.1f} Hz")

    # Tone changes, which only happen in the 1010 test.
    changes = np.flatnonzero(np.diff(is_space.astype(np.int8)) != 0)

    if len(changes) >= 20:
        span = (at[-1] - at[0]) / FS2
        rate = len(changes) / span

        print(f"\n{len(changes)} tone changes in {span * 1000:.0f} ms")
        print(f"  change rate {verdict(rate, BAUD, 40.0, 'Hz')}")
        print("  In the 1010 pattern every bit changes tone, so this is the bit")
        print("  rate. It is an indicative figure: half cycles that straddle a")
        print("  change get counted on one side or the other. Whether the timing")
        print("  is really good enough is settled by direwolf decoding a beacon,")
        print("  not by this number.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--freq", type=float, default=144_800_000.0)
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="fixed capture window; default 0 waits for a burst")
    ap.add_argument("--minutes", type=float, default=5.0)
    ap.add_argument("--gain", default="20", help="tuner gain in dB; lower it if overloading")
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--mark", type=float, default=MARK_HZ)
    ap.add_argument("--space", type=float, default=SPACE_HZ)
    ap.add_argument("--baud", type=float, default=BAUD)
    # Saving the burst means the analysis can be reworked without asking the
    # operator to transmit again, which during bring-up is most of the cost.
    ap.add_argument("--save", help="write the captured burst to this file")
    ap.add_argument("--load", help="analyse a saved burst instead of capturing")
    ap.add_argument("--wav", help="write the demodulated audio here, for direwolf")
    ap.add_argument("--window", type=float, default=1.0,
                    help="seconds of burst to keep; widen for a data frame")
    args = ap.parse_args()

    set_tones(args.mark, args.space, args.baud)

    tune = int(args.freq - OFFSET_HZ)

    if args.load:
        burst = np.fromfile(args.load, dtype=np.complex64)

        if args.wav:
            secs = write_wav(burst, args.wav)
            print(f"wrote {args.wav}, {secs:.2f}s at {WAV_RATE} Hz")

        report(burst, tune)
        return 0

    print(f"listening on {args.freq / 1e6:.4f} MHz, tuned {tune / 1e6:.4f} "
          f"so the DC spike is clear of the signal")

    if args.seconds > 0:
        burst = strongest_window(capture(args.seconds, args.gain, tune, args.device),
                                 int(SAMPLE_RATE * args.window))
    else:
        burst = wait_for_burst(args.gain, tune, args.device, minutes=args.minutes,
                               window=args.window)

        if burst is None:
            return 1

    if args.save:
        burst.astype(np.complex64).tofile(args.save)
        print(f"burst saved to {args.save}")

    if args.wav:
        secs = write_wav(burst, args.wav)
        print(f"wrote {args.wav}, {secs:.2f}s at {WAV_RATE} Hz")

    report(burst, tune)

    return 0


if __name__ == "__main__":
    sys.exit(main())
