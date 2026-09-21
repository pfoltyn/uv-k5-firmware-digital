#!/bin/sh
# The Phase 1 gate.
#
# The BK4819's FFSK mode gives a 1200/2400 tone pair; Bell 202, which every TNC
# and igate expects, is 1200/2200. REG_72 sets the mark tone and the mode fixes
# the ratio, so 2200 cannot be had without dragging the mark and the bit rate
# down with it.
#
# Whether that 200Hz matters is decidable here, with no radio, no licence and no
# second station: render the same frame at a range of space tones and see which
# ones stock direwolf decodes.
#
# Reported as a decode rate against added noise rather than as a yes or no,
# because a clean signal answers the wrong question. What matters is how much
# room there is either side of 2400, since the chip's real tone will not be
# exactly 2400 and Phase 2 has to measure it.
#
# If 2400 turns out to be marginal, the alternative is exact Bell 202 in
# software: drive TONE2 alone and rewrite REG_72 between 0x3065 (1200Hz) and
# 0x58BA (2200Hz) every 833us.

ATEST=${ATEST:-atest}
WAV=${WAV:-./aprs_wav}
SNR=${SNR:-10}
TRIALS=${TRIALS:-5}

SPEC=${SPEC:-'SP9ABC-7>APZK5F,WIDE1-1,WIDE2-1:=5001.25N/01957.50E-UV-K5 APRS'}

strip() { sed -E 's/\x1b\[[0-9;]*[mJK]//g'; }

# Repeats with a different lead-in each time, which shifts the frame against the
# decoder's sample phase. A single trial hides alignment sensitivity.
rate() {
    mark=$1
    space=$2
    ok=0
    t=1

    while [ "$t" -le "$TRIALS" ]; do
        $WAV --mark "$mark" --space "$space" --snr "$SNR" \
             --lead-ms $((50 + t * 7)) -o tone.wav "$SPEC" 2>/dev/null

        got=$($ATEST tone.wav 2>/dev/null | strip | sed -n 's/^\[0\] //p' | head -1)

        [ "$got" = "$SPEC" ] && ok=$((ok + 1))
        t=$((t + 1))
    done

    echo "$ok"
}

echo "frame: $SPEC"
echo "$TRIALS trials per tone at ${SNR}dB SNR, decoded by stock direwolf"
echo
printf '%-6s %-6s %-8s %s\n' mark space decoded notes
printf '%-6s %-6s %-8s %s\n' ---- ----- ------- -----

at_2400=0

for pair in "1200 2200 Bell 202, the reference" \
            "1200 2250 -" \
            "1200 2300 -" \
            "1200 2350 -" \
            "1200 2375 -" \
            "1200 2400 the BK4819 FFSK 1200/2400 mode" \
            "1200 2425 -" \
            "1200 2450 -" \
            "1200 2500 -" \
            "1200 1800 the BK4819 FFSK 1200/1800 mode"; do
    mark=$(echo "$pair" | cut -d' ' -f1)
    space=$(echo "$pair" | cut -d' ' -f2)
    note=$(echo "$pair" | cut -d' ' -f3-)

    ok=$(rate "$mark" "$space")

    [ "$space" = 2400 ] && at_2400=$ok
    [ "$note" = "-" ] && note=""

    printf '%-6s %-6s %-8s %s\n' "$mark" "$space" "$ok/$TRIALS" "$note"
done

rm -f tone.wav

# A single demodulator answers only for itself. direwolf ships several and real
# receivers are a mix of them and of hardware TNCs, so sweep the ones that work
# at all: if 2400 depends on which demodulator the far end runs, a transmit-only
# feature aimed at arbitrary igates cannot rely on it.
echo
echo "the same question per demodulator, at the two tones that matter"
echo
printf '%-10s %-8s %-8s\n' demod 2200 2400
printf '%-10s %-8s %-8s\n' ----- ---- ----

for p in A B D E; do
    printf '%-10s ' "-P $p"
    for space in 2200 2400; do
        ok=0
        t=1
        while [ "$t" -le "$TRIALS" ]; do
            $WAV --mark 1200 --space "$space" --snr "$SNR" \
                 --lead-ms $((50 + t * 7)) -o tone.wav "$SPEC" 2>/dev/null
            got=$($ATEST -P "$p" tone.wav 2>/dev/null | strip \
                  | sed -n 's/^\[0\] //p' | head -1)
            [ "$got" = "$SPEC" ] && ok=$((ok + 1))
            t=$((t + 1))
        done
        printf '%-8s ' "$ok/$TRIALS"
    done
    echo
done

rm -f tone.wav

echo
if [ "$at_2400" -eq "$TRIALS" ]; then
    echo "GATE PASSED: 2400Hz decodes every time on the default demodulator."
elif [ "$at_2400" -gt 0 ]; then
    echo "GATE PASSED, NARROWLY: 2400Hz decoded $at_2400 of $TRIALS on the default"
    echo "demodulator, and the table above shows the answer depends on which"
    echo "demodulator the receiver runs. Good enough to prove the software chain,"
    echo "not good enough to rely on for reaching arbitrary igates, so bit-banged"
    echo "Bell 202 should be the primary transmit path and FFSK the cheap extra."
else
    echo "GATE FAILED: 2400Hz does not decode on a stock TNC. Transmit needs"
    echo "bit-banged Bell 202 (REG_72 between 0x3065 and 0x58BA at 833us per bit)"
    echo "rather than the chip's FFSK mode."
    exit 1
fi
