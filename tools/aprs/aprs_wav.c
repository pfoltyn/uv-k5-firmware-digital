/* Host-side harness for the AX.25 encoder in app/ax25.c and app/aprs.c.
 *
 * Renders a frame as AFSK audio so direwolf can decode it, with the mark and
 * space tones on the command line. That is the point of this tool: the BK4819's
 * FFSK mode gives 1200/2400, not Bell 202's 1200/2200, and whether a standard
 * TNC accepts the difference is answerable here with no radio involved.
 *
 * Takes a TNC2 monitor string, the same form direwolf's gen_packets reads, so
 * the same text can be put through both and the results compared.
 *
 * This file is host tooling only, it is never compiled into the firmware.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/aprs.h"
#include "app/ax25.h"

#define DEFAULT_RATE   44100
#define DEFAULT_BAUD   1200
#define DEFAULT_MARK   1200
#define DEFAULT_SPACE  2200    // Bell 202. The BK4819's FFSK gives 2400.

struct options_t {
    uint32_t    rate;
    uint32_t    baud;
    uint32_t    mark;
    uint32_t    space;
    uint32_t    lead_ms;
    uint32_t    tail_ms;
    uint8_t     lead_flags;
    uint8_t     alt_bytes;
    double      amplitude;
    double      snr_db;      // 0 for none
    bool        hex;
    bool        air;
    const char *out;
    const char *spec;
};

/* White Gaussian noise, so the tone comparison can be made at a stated signal
 * to noise ratio rather than only on a clean signal. Lowering the amplitude is
 * not a substitute: direwolf normalises level, so a quiet clean signal decodes
 * exactly as well as a loud one and says nothing about margin. */
struct noise_t {
    double   sigma;
    uint32_t seed;
    double   spare;
    bool     have_spare;
};

static double Uniform(struct noise_t *n)
{
    n->seed = n->seed * 1103515245u + 12345u;

    return ((double)(n->seed >> 8) / 16777216.0);   // 0..1
}

static double Gaussian(struct noise_t *n)
{
    if (n->have_spare) {
        n->have_spare = false;
        return n->spare;
    }

    double u, v, s;

    do {
        u = 2.0 * Uniform(n) - 1.0;
        v = 2.0 * Uniform(n) - 1.0;
        s = u * u + v * v;
    } while (s >= 1.0 || s == 0.0);

    const double f = sqrt(-2.0 * log(s) / s);

    n->spare      = v * f;
    n->have_spare = true;

    return u * f;
}

static void WriteLE(FILE *f, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; i++)
        fputc((value >> (8 * i)) & 0xFF, f);
}

static void WriteWavHeader(FILE *f, uint32_t rate, uint32_t samples)
{
    const uint32_t data_bytes = samples * 2;

    fwrite("RIFF", 1, 4, f);
    WriteLE(f, 36 + data_bytes, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    WriteLE(f, 16, 4);          // fmt chunk size
    WriteLE(f, 1, 2);           // PCM
    WriteLE(f, 1, 2);           // mono
    WriteLE(f, rate, 4);
    WriteLE(f, rate * 2, 4);    // byte rate
    WriteLE(f, 2, 2);           // block align
    WriteLE(f, 16, 2);          // bits per sample
    fwrite("data", 1, 4, f);
    WriteLE(f, data_bytes, 4);
}

/* "SRC>DEST,DIGI,DIGI:info", which is what direwolf prints and gen_packets
 * reads. info is copied out because the frame holds only a pointer. */
static bool ParseTnc2(const char *spec, AX25_Frame_t *frame,
                      char *info, size_t info_max)
{
    char        field[32];
    const char *p = spec;
    size_t      n = 0;

    memset(frame, 0, sizeof *frame);

    while (*p != '\0' && *p != '>') {
        if (n + 1 >= sizeof field)
            return false;
        field[n++] = *p++;
    }

    if (*p != '>')
        return false;

    field[n] = '\0';

    if (!AX25_ParseAddr(field, &frame->src)) {
        fprintf(stderr, "bad source callsign '%s'\n", field);
        return false;
    }

    bool have_dest = false;

    n = 0;

    for (p++; ; p++) {
        if (*p != ',' && *p != ':' && *p != '\0') {
            if (n + 1 >= sizeof field)
                return false;
            field[n++] = *p;
            continue;
        }

        field[n] = '\0';
        n        = 0;

        AX25_Addr_t addr;

        if (!AX25_ParseAddr(field, &addr)) {
            fprintf(stderr, "bad address '%s'\n", field);
            return false;
        }

        if (!have_dest) {
            frame->dest = addr;
            have_dest   = true;
        } else {
            if (frame->digis >= AX25_MAX_DIGIS) {
                fprintf(stderr, "more than %d digipeaters\n", AX25_MAX_DIGIS);
                return false;
            }
            frame->digi[frame->digis++] = addr;
        }

        if (*p != ',')
            break;
    }

    if (*p != ':')
        return false;

    p++;

    if (strlen(p) >= info_max)
        return false;

    strcpy(info, p);
    frame->info = info;

    return have_dest;
}

static void EmitSample(FILE *f, double value, struct noise_t *n, uint32_t *written)
{
    double v = value;

    if (n->sigma > 0.0)
        v += n->sigma * Gaussian(n);

    if (v > 32767.0)  v = 32767.0;
    if (v < -32768.0) v = -32768.0;

    WriteLE(f, (uint16_t)(int16_t)v, 2);
    (*written)++;
}

static void RenderTone(FILE *f, uint32_t rate, uint32_t hz, uint32_t samples,
                       double amplitude, double *phase, struct noise_t *n,
                       uint32_t *written)
{
    const double step = 2.0 * M_PI * (double)hz / (double)rate;

    for (uint32_t i = 0; i < samples; i++) {
        EmitSample(f, amplitude * 32767.0 * sin(*phase), n, written);

        *phase += step;

        if (*phase > 2.0 * M_PI)
            *phase -= 2.0 * M_PI;
    }
}

static void Usage(void)
{
    fprintf(stderr,
        "usage: aprs_wav [options] \"SRC>DEST,PATH:info\"\n"
        "  -o FILE        output WAV (default aprs.wav)\n"
        "  --rate N       sample rate (default %u)\n"
        "  --baud N       bit rate (default %u)\n"
        "  --mark N       mark tone (default %u)\n"
        "  --space N      space tone (default %u, the BK4819's FFSK gives 2400)\n"
        "  --flags N      lead flags (default %u)\n"
        "  --alt N        bytes of alternating preamble before the flags, for the\n"
        "                 BK4819's own receiver; standard TNCs skip it (default %u)\n"
        "  --lead-ms N    silence before the frame (default 100)\n"
        "  --tail-ms N    silence after (default 100)\n"
        "  --amplitude F  0..1 (default 0.5)\n"
        "  --snr DB       add white noise at this signal to noise ratio\n"
        "  --hex          print the frame bytes without the FCS, as atest -h does\n"
        "  --air          print the on-air bytes after stuffing and NRZI\n",
        DEFAULT_RATE, DEFAULT_BAUD, DEFAULT_MARK, DEFAULT_SPACE, AX25_LEAD_FLAGS,
        AX25_LEAD_ALT);
}

int main(int argc, char **argv)
{
    struct options_t opt = {
        .rate = DEFAULT_RATE, .baud = DEFAULT_BAUD,
        .mark = DEFAULT_MARK, .space = DEFAULT_SPACE,
        .lead_ms = 100, .tail_ms = 100,
        .lead_flags = AX25_LEAD_FLAGS, .alt_bytes = AX25_LEAD_ALT,
        .amplitude = 0.5,
        .out = "aprs.wav",
    };

    for (int i = 1; i < argc; i++) {
        const char *a    = argv[i];
        const bool  more = (i + 1 < argc);

        if (!strcmp(a, "-o") && more)                   opt.out = argv[++i];
        else if (!strcmp(a, "--rate") && more)          opt.rate = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--baud") && more)          opt.baud = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--mark") && more)          opt.mark = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--space") && more)         opt.space = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--flags") && more)         opt.lead_flags = (uint8_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--alt") && more)           opt.alt_bytes = (uint8_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--lead-ms") && more)       opt.lead_ms = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--tail-ms") && more)       opt.tail_ms = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--amplitude") && more)     opt.amplitude = strtod(argv[++i], NULL);
        else if (!strcmp(a, "--snr") && more)           opt.snr_db = strtod(argv[++i], NULL);
        else if (!strcmp(a, "--hex"))                   opt.hex = true;
        else if (!strcmp(a, "--air"))                   opt.air = true;
        else if (a[0] == '-' && a[1] != '\0')         { Usage(); return 2; }
        else                                            opt.spec = a;
    }

    if (opt.spec == NULL || opt.baud == 0 || opt.rate == 0) {
        Usage();
        return 2;
    }

    AX25_Frame_t frame;
    char         info[AX25_MAX_INFO + 1];

    if (!ParseTnc2(opt.spec, &frame, info, sizeof info))
        return 1;

    uint8_t        raw[AX25_MAX_FRAME];
    const uint32_t len = AX25_BuildFrame(&frame, raw, sizeof raw);

    if (len == 0) {
        fprintf(stderr, "frame would not build (callsign or length)\n");
        return 1;
    }

    uint8_t     storage[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;

    AX25_BitsInit(&bits, storage, sizeof storage);

    if (!AX25_ToAir(raw, len, opt.alt_bytes, opt.lead_flags, &bits)) {
        fprintf(stderr, "air buffer overflowed\n");
        return 1;
    }

    if (opt.hex) {
        // atest -h prints the frame without the FCS, so match that exactly.
        for (uint32_t i = 0; i + 2 < len; i++)
            printf("%02x%s", raw[i], (i + 3 == len) ? "\n" : " ");
    }

    if (opt.air)
        for (uint32_t i = 0; i < bits.length / 8; i++)
            printf("%02x%s", storage[i], ((i + 1) * 8 == bits.length) ? "\n" : " ");

    FILE *f = fopen(opt.out, "wb");

    if (f == NULL) {
        perror(opt.out);
        return 1;
    }

    WriteWavHeader(f, opt.rate, 0);   // patched once the length is known

    uint32_t written = 0;
    double   phase   = 0.0;

    /* A sine of amplitude A has mean power A^2/2, so the noise standard
     * deviation for a wanted SNR follows directly. Noise runs through the lead
     * and tail silence too, otherwise the decoder would see an impossibly clean
     * channel either side of the burst. */
    struct noise_t noise = { 0.0, 1u, 0.0, false };

    if (opt.snr_db != 0.0) {
        const double signal = (opt.amplitude * 32767.0) * (opt.amplitude * 32767.0) / 2.0;

        noise.sigma = sqrt(signal / pow(10.0, opt.snr_db / 10.0));
    }

    for (uint32_t i = 0; i < opt.rate * opt.lead_ms / 1000; i++)
        EmitSample(f, 0.0, &noise, &written);

    /* Fractional samples per bit, accumulated rather than rounded: 44100/1200
     * is 36.75, and rounding each bit to 37 samples would run 0.7% slow, which
     * a long frame notices. */
    for (uint32_t i = 0; i < bits.length; i++) {
        const uint32_t from = (uint32_t)((uint64_t)i * opt.rate / opt.baud);
        const uint32_t to   = (uint32_t)((uint64_t)(i + 1) * opt.rate / opt.baud);

        // The NRZI level selects the tone: a 1 is mark, a 0 is space.
        RenderTone(f, opt.rate, AX25_BitsGet(&bits, i) ? opt.mark : opt.space,
                   to - from, opt.amplitude, &phase, &noise, &written);
    }

    for (uint32_t i = 0; i < opt.rate * opt.tail_ms / 1000; i++)
        EmitSample(f, 0.0, &noise, &written);

    fseek(f, 0, SEEK_SET);
    WriteWavHeader(f, opt.rate, written);
    fclose(f);

    fprintf(stderr, "%s: %u frame bytes, %u air bits, %u/%u Hz at %u baud, %.2fs",
            opt.out, len, bits.length, opt.mark, opt.space, opt.baud,
            (double)written / (double)opt.rate);

    if (opt.snr_db != 0.0)
        fprintf(stderr, ", %.0fdB SNR", opt.snr_db);

    fprintf(stderr, "\n");

    return 0;
}
