#!/bin/sh
# Puts frames from app/ax25.c through direwolf and checks they come back
# unchanged, which tests addresses, bit stuffing, NRZI and the FCS at once: any
# of them wrong and atest reports nothing at all.
#
# Also compares our frame bytes against direwolf's own encoder for the same
# packet, so a difference shows up as a byte rather than as a silent decode.

set -e

ATEST=${ATEST:-atest}
GEN_PACKETS=${GEN_PACKETS:-gen_packets}
WAV=${WAV:-./aprs_wav}

# atest colours its output, which would otherwise end up inside the comparison
strip() { sed -E 's/\x1b\[[0-9;]*[mJK]//g'; }

fail=0

check() {
    spec="$1"

    $WAV -o rt.wav "$spec" 2>/dev/null

    got=$($ATEST rt.wav 2>/dev/null | strip | sed -n 's/^\[0\] //p' | head -1)

    if [ "$got" = "$spec" ]; then
        echo "  ok    $spec"
    else
        echo "  FAIL  sent '$spec'"
        echo "        got  '$got'"
        fail=$((fail + 1))
    fi
}

echo "-- our encoder, decoded by direwolf --"
check 'SP9ABC-7>APZK5F:>UV-K5 status'
check 'SP9ABC-7>APZK5F,WIDE1-1,WIDE2-1:>digipeated status'
check 'SP9ABC-7>APZK5F,WIDE2-1:=5001.25N/01957.50E-UV-K5'
check 'W1AW>APRS:>no ssid anywhere'
check '2E0XYZ-15>APZK5F,WIDE1-1:hello with the maximum ssid'
check 'SP9ABC>APZK5F::W1AW     :message text{01'
# Deliberately full of the 0x7F and 0xFF-ish bytes that force bit stuffing
check 'SP9ABC>APZK5F:>~~~~~~~~~~~~~~~~ oooooooo'

echo "-- our frame bytes vs direwolf's own encoder --"

# gen_packets builds the frame from a text file and atest -h prints the frame it
# decoded, without the FCS. Both sides go through atest -h, so what is compared
# is the same thing encoded twice.
#
# printf without a trailing newline matters: gen_packets would otherwise put the
# file's newline in the information field and every comparison would differ by a
# trailing 0a. Two spaces separate atest's hex column from its ASCII one, which
# is what awk splits on.
cmp_bytes() {
    spec="$1"

    printf '%s' "$spec" > rt.txt
    $GEN_PACKETS -n 1 -o rt_dw.wav rt.txt >/dev/null 2>&1
    theirs=$($ATEST -h rt_dw.wav 2>/dev/null | strip \
             | sed -n 's/^ *[0-9a-f]\{3\}: *//p' | awk -F'  ' '{print $1}' \
             | tr '\n' ' ' | tr -s ' ' | sed 's/ *$//')

    ours=$($WAV --hex -o rt.wav "$spec" 2>/dev/null | tr -s ' ' | sed 's/ *$//')

    if [ "$theirs" = "$ours" ]; then
        echo "  ok    $spec"
    else
        echo "  FAIL  $spec"
        echo "        ours     $ours"
        echo "        direwolf $theirs"
        fail=$((fail + 1))
    fi
}

cmp_bytes 'SP9ABC-7>APZK5F,WIDE1-1,WIDE2-1:>hello'
cmp_bytes 'W1AW>APRS:=5001.25N/01957.50E-home'

rm -f rt.wav rt_dw.wav rt.txt

if [ "$fail" -ne 0 ]; then
    echo "$fail failures"
    exit 1
fi

echo "all round trips passed"
