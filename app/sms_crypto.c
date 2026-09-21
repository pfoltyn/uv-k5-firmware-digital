/* Authenticated encryption for SMS. See sms_crypto.h. */

#include <stddef.h>   // NULL

#include "app/sms_crypto.h"

#ifdef SMS_CRYPTO_HOST_TEST
    extern uint32_t SMS_CryptoHostSeed(void);
    #define CRYPTO_SEED() SMS_CryptoHostSeed()
#else
    #include "ARMCM0.h"
    #include "bsp/dp32g030/syscon.h"

    /* Seeded from SysTick and the chip ID rather than persisted. SysTick is free
     * running, so its value at the moment the key is set is effectively arbitrary
     * relative to power-on, which is what stops two boots starting from the same
     * counter. It is not a guarantee; see the header. */
    static uint32_t CRYPTO_SEED(void)
    {
        return (uint32_t)SysTick->VAL ^ SYSCON_CHIP_ID0 ^ SYSCON_CHIP_ID3;
    }
#endif

// --- ChaCha20 --------------------------------------------------------------

static uint32_t Rotl(uint32_t v, unsigned n)
{
    return (uint32_t)((v << n) | (v >> (32 - n)));
}

/* A function rather than a macro, deliberately. As a macro this expands eight times
 * per double round and cost about 500 bytes of flash for call overhead nobody can
 * measure on a 42 byte message. The same reasoning applies to SipRound below, and
 * between them it is most of a kilobyte on a budget of eight. */
static void QR(uint32_t *x, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    x[a] += x[b];  x[d] ^= x[a];  x[d] = Rotl(x[d], 16);
    x[c] += x[d];  x[b] ^= x[c];  x[b] = Rotl(x[b], 12);
    x[a] += x[b];  x[d] ^= x[a];  x[d] = Rotl(x[d],  8);
    x[c] += x[d];  x[b] ^= x[c];  x[b] = Rotl(x[b],  7);
}

static uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ChaChaBlock(const uint8_t *key, uint32_t counter,
                        const uint8_t *nonce, uint8_t *out)
{
    // "expand 32-byte k", the RFC 8439 constants.
    static const uint32_t c[4] = { 0x61707865u, 0x3320646Eu, 0x79622D32u, 0x6B206574u };

    uint32_t s[16];
    uint32_t x[16];

    s[0] = c[0]; s[1] = c[1]; s[2] = c[2]; s[3] = c[3];

    for (uint8_t i = 0; i < 8; i++)
        s[4 + i] = Le32(key + i * 4);

    s[12] = counter;
    s[13] = Le32(nonce + 0);
    s[14] = Le32(nonce + 4);
    s[15] = Le32(nonce + 8);

    for (uint8_t i = 0; i < 16; i++)
        x[i] = s[i];

    // Twenty rounds, as ten column-then-diagonal pairs.
    for (uint8_t i = 0; i < 10; i++) {
        QR(x, 0, 4,  8, 12);
        QR(x, 1, 5,  9, 13);
        QR(x, 2, 6, 10, 14);
        QR(x, 3, 7, 11, 15);

        QR(x, 0, 5, 10, 15);
        QR(x, 1, 6, 11, 12);
        QR(x, 2, 7,  8, 13);
        QR(x, 3, 4,  9, 14);
    }

    for (uint8_t i = 0; i < 16; i++) {
        const uint32_t v = x[i] + s[i];

        out[i * 4 + 0] = (uint8_t)(v & 0xFFu);
        out[i * 4 + 1] = (uint8_t)((v >> 8) & 0xFFu);
        out[i * 4 + 2] = (uint8_t)((v >> 16) & 0xFFu);
        out[i * 4 + 3] = (uint8_t)((v >> 24) & 0xFFu);
    }
}

void SMS_ChaCha20(const uint8_t *key, uint32_t counter, const uint8_t *nonce,
                  uint8_t *buf, uint32_t len)
{
    uint8_t  block[64];
    uint32_t done = 0;

    while (done < len) {
        ChaChaBlock(key, counter, nonce, block);
        counter++;

        uint32_t n = len - done;

        if (n > 64)
            n = 64;

        for (uint32_t i = 0; i < n; i++)
            buf[done + i] ^= block[i];

        done += n;
    }
}

// --- SipHash-2-4 -----------------------------------------------------------

/* 64 bit arithmetic without a 64x64 multiply, which is the whole reason this is
 * here rather than Poly1305: on ARMv6-M each of these is a pair of 32 bit
 * instructions and there is nothing to synthesise. */
static uint64_t Rotl64(uint64_t v, unsigned n)
{
    return (uint64_t)((v << n) | (v >> (64 - n)));
}

// A function for the same reason as QR: eight expansions of 64 bit shifts and adds
// is expensive on a core that has to synthesise all of them from 32 bit ops.
static void SipRound(uint64_t *v)
{
    v[0] += v[1]; v[1] = Rotl64(v[1], 13); v[1] ^= v[0]; v[0] = Rotl64(v[0], 32);
    v[2] += v[3]; v[3] = Rotl64(v[3], 16); v[3] ^= v[2];
    v[0] += v[3]; v[3] = Rotl64(v[3], 21); v[3] ^= v[0];
    v[2] += v[1]; v[1] = Rotl64(v[1], 17); v[1] ^= v[2]; v[2] = Rotl64(v[2], 32);
}

static uint64_t Le64(const uint8_t *p)
{
    uint64_t v = 0;

    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];

    return v;
}

uint64_t SMS_SipHash(const uint8_t *key, const uint8_t *data, uint32_t len)
{
    const uint64_t k0 = Le64(key);
    const uint64_t k1 = Le64(key + 8);

    uint64_t v[4] = {
        k0 ^ 0x736F6D6570736575ull,
        k1 ^ 0x646F72616E646F6Dull,
        k0 ^ 0x6C7967656E657261ull,
        k1 ^ 0x7465646279746573ull
    };

    const uint32_t whole = len & ~7u;

    for (uint32_t off = 0; off < whole; off += 8) {
        const uint64_t m = Le64(data + off);

        v[3] ^= m;
        SipRound(v);
        SipRound(v);
        v[0] ^= m;
    }

    // The last block is the remaining bytes with the length in its top byte.
    uint64_t last = (uint64_t)(len & 0xFFu) << 56;

    for (uint32_t i = 0; i < (len & 7u); i++)
        last |= (uint64_t)data[whole + i] << (i * 8);

    v[3] ^= last;
    SipRound(v);
    SipRound(v);
    v[0] ^= last;

    v[2] ^= 0xFFu;

    for (uint8_t i = 0; i < 4; i++)
        SipRound(v);

    return v[0] ^ v[1] ^ v[2] ^ v[3];
}

// --- the wrapper -----------------------------------------------------------

static uint8_t  gKey[32];
static uint8_t  gMacKey[16];
static bool     gHaveKey;
static uint32_t gCounter;

void SMS_Crypto_SetKey(const char *passphrase)
{
    uint8_t  buf[64];
    uint32_t n = 0;

    gHaveKey = false;

    if (passphrase == NULL || passphrase[0] == '\0')
        return;

    for (uint32_t i = 0; i < sizeof buf; i++)
        buf[i] = 0;

    while (passphrase[n] != '\0' && n < sizeof buf)
        n++;

    for (uint32_t i = 0; i < n; i++)
        buf[i] = (uint8_t)passphrase[i];

    /* Iterated ChaCha20 over the passphrase. Each round encrypts the buffer under
     * itself, so the work cannot be skipped ahead; a few thousand rounds costs
     * about 40ms here and multiplies the cost of guessing by the same factor. It is
     * a speed bump, not scrypt - see the header. */
    static const uint8_t zero_nonce[12] = { 0 };

    for (uint32_t round = 0; round < 3000u; round++)
        SMS_ChaCha20(buf, round, zero_nonce, buf + 32, 32);

    for (uint8_t i = 0; i < 32; i++)
        gKey[i] = buf[i] ^ buf[32 + i];

    // A separate MAC key, so the tag never uses the cipher key directly.
    SMS_ChaCha20(gKey, 0xFFFFFFFFu, zero_nonce, buf, 16);

    for (uint8_t i = 0; i < 16; i++)
        gMacKey[i] = buf[i];

    gCounter = CRYPTO_SEED();
    gHaveKey = true;
}

bool SMS_Crypto_HaveKey(void)
{
    return gHaveKey;
}

// Builds the 96 bit ChaCha nonce. The counter alone is not enough: the same
// message's fragments share it, so the fragment index has to be in here or two
// fragments would use the same keystream.
static void BuildNonce(uint8_t *out, uint32_t counter, uint16_t src,
                       uint8_t msg_id, uint8_t frag)
{
    out[0]  = (uint8_t)(counter & 0xFFu);
    out[1]  = (uint8_t)((counter >> 8) & 0xFFu);
    out[2]  = (uint8_t)((counter >> 16) & 0xFFu);
    out[3]  = (uint8_t)((counter >> 24) & 0xFFu);
    out[4]  = (uint8_t)(src & 0xFFu);
    out[5]  = (uint8_t)(src >> 8);
    out[6]  = msg_id;
    out[7]  = frag;
    out[8]  = 0;
    out[9]  = 0;
    out[10] = 0;
    out[11] = 0;
}

/* What the tag covers: the context, the nonce, then the ciphertext.
 *
 * The context is the part that is easy to leave out and was, until a test caught
 * it. src, msg_id and frag travel in the frame header in the clear, and they go
 * into the ChaCha nonce so they affect the keystream - but affecting the keystream
 * is not authentication. With the tag over the ciphertext alone, an attacker could
 * relabel a frame as a different sender, message or fragment: the tag still
 * verified, the plaintext came out as garbage, and the receiver accepted the
 * garbage as genuine. They are associated data and have to be in here.
 */
static uint32_t MacInput(uint8_t *out, const uint8_t *payload, uint8_t text_len,
                         uint16_t src, uint8_t msg_id, uint8_t frag)
{
    uint32_t n = 0;

    out[n++] = (uint8_t)(src & 0xFFu);
    out[n++] = (uint8_t)(src >> 8);
    out[n++] = msg_id;
    out[n++] = frag;

    for (uint8_t i = 0; i < SMS_NONCE_BYTES; i++)
        out[n++] = payload[i];

    for (uint8_t i = 0; i < text_len; i++)
        out[n++] = payload[SMS_CRYPTO_OVERHEAD + i];

    return n;
}

static void PutTag(uint8_t *dst, uint64_t tag)
{
    for (uint8_t i = 0; i < 8; i++)
        dst[i] = (uint8_t)((tag >> (i * 8)) & 0xFFu);
}

uint8_t SMS_Crypto_Seal(uint8_t *payload, uint8_t text_len,
                        uint16_t src, uint8_t msg_id, uint8_t frag)
{
    uint8_t nonce[12];

    if (!gHaveKey)
        return text_len;

    const uint32_t counter = gCounter++;

    // Shift the plaintext up to leave room for the nonce and tag.
    for (int16_t i = (int16_t)text_len - 1; i >= 0; i--)
        payload[SMS_CRYPTO_OVERHEAD + i] = payload[i];

    payload[0] = (uint8_t)(counter & 0xFFu);
    payload[1] = (uint8_t)((counter >> 8) & 0xFFu);
    payload[2] = (uint8_t)((counter >> 16) & 0xFFu);
    payload[3] = (uint8_t)((counter >> 24) & 0xFFu);

    BuildNonce(nonce, counter, src, msg_id, frag);

    // Block counter 1, as RFC 8439 uses for payload data.
    SMS_ChaCha20(gKey, 1, nonce, payload + SMS_CRYPTO_OVERHEAD, text_len);

    // Encrypt then MAC, over the context, the nonce and the ciphertext.
    uint8_t        mac_in[8 + 64];
    const uint32_t mac_len = MacInput(mac_in, payload, text_len, src, msg_id, frag);

    PutTag(payload + SMS_NONCE_BYTES, SMS_SipHash(gMacKey, mac_in, mac_len));

    return (uint8_t)(text_len + SMS_CRYPTO_OVERHEAD);
}

bool SMS_Crypto_Open(uint8_t *payload, uint8_t *len,
                     uint16_t src, uint8_t msg_id, uint8_t frag)
{
    uint8_t nonce[12];

    if (!gHaveKey)
        return true;

    if (len == NULL || *len < SMS_CRYPTO_OVERHEAD)
        return false;

    const uint8_t text_len = (uint8_t)(*len - SMS_CRYPTO_OVERHEAD);

    uint8_t        mac_in[8 + 64];
    const uint32_t mac_len = MacInput(mac_in, payload, text_len, src, msg_id, frag);
    const uint64_t want    = SMS_SipHash(gMacKey, mac_in, mac_len);
    uint8_t        tag[8];

    PutTag(tag, want);

    /* Constant time comparison. A byte-at-a-time early exit would leak how much of
     * a forged tag was right, which is enough to find the rest one byte at a
     * time. */
    uint8_t diff = 0;

    for (uint8_t i = 0; i < SMS_TAG_BYTES; i++)
        diff |= (uint8_t)(payload[SMS_NONCE_BYTES + i] ^ tag[i]);

    if (diff != 0)
        return false;

    const uint32_t counter = (uint32_t)payload[0] |
                             ((uint32_t)payload[1] << 8) |
                             ((uint32_t)payload[2] << 16) |
                             ((uint32_t)payload[3] << 24);

    BuildNonce(nonce, counter, src, msg_id, frag);
    SMS_ChaCha20(gKey, 1, nonce, payload + SMS_CRYPTO_OVERHEAD, text_len);

    // Shift the plaintext back down to the start.
    for (uint8_t i = 0; i < text_len; i++)
        payload[i] = payload[SMS_CRYPTO_OVERHEAD + i];

    *len = text_len;

    return true;
}
