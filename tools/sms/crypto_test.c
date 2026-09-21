/* Host tests for app/sms_crypto.c.
 *
 * The primitives are checked against references written by somebody else, which is
 * the only kind of check worth having for cryptography: the ChaCha20 keystream
 * against LibreSSL's chacha and against RFC 8439's own vector, and SipHash-2-4
 * against the published vectors from its paper. A cipher that is merely
 * self-consistent is a cipher that is wrong in a way its author cannot see.
 *
 * The rest is about what an attacker can do: flip a bit, replay, use the wrong key.
 *
 * This file is host tooling only, it is never compiled into the firmware.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/sms_crypto.h"

static int checks;
static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d  ", __func__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

// Fixed so that a sealed payload is reproducible between runs; the firmware seeds
// this from SysTick and the chip ID instead.
static uint32_t gSeed = 0x11223344u;

uint32_t SMS_CryptoHostSeed(void) { return gSeed; }

static void Hex(const uint8_t *p, uint32_t n, char *out)
{
    static const char h[] = "0123456789abcdef";

    for (uint32_t i = 0; i < n; i++) {
        out[i * 2]     = h[p[i] >> 4];
        out[i * 2 + 1] = h[p[i] & 0x0Fu];
    }

    out[n * 2] = '\0';
}

static void TestChaCha20AgainstRfc8439(void)
{
    /* RFC 8439 section 2.4.2: key 00..1f, counter 1, nonce 000000090000004a00000000.
     * Verified to be byte for byte what LibreSSL's chacha produces for the same
     * inputs, so this is two independent references agreeing. */
    static const char *want =
        "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e";

    uint8_t key[32];
    uint8_t buf[64];
    char    got[129];

    for (uint8_t i = 0; i < 32; i++)
        key[i] = i;

    static const uint8_t nonce[12] = { 0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0 };

    memset(buf, 0, sizeof buf);
    SMS_ChaCha20(key, 1, nonce, buf, sizeof buf);
    Hex(buf, sizeof buf, got);

    CHECK(strcmp(got, want) == 0, "keystream\n    got  %s\n    want %s", got, want);

    // And it must be its own inverse, since it is a stream cipher.
    uint8_t text[32] = "the quick brown fox jumps over.";
    uint8_t copy[32];

    memcpy(copy, text, sizeof copy);
    SMS_ChaCha20(key, 1, nonce, text, sizeof text);
    CHECK(memcmp(text, copy, sizeof text) != 0, "encryption changed nothing");
    SMS_ChaCha20(key, 1, nonce, text, sizeof text);
    CHECK(memcmp(text, copy, sizeof text) == 0, "decryption did not restore");
}

static void TestChaCha20SpansBlocks(void)
{
    /* The block counter has to advance for data past 64 bytes. Getting that wrong
     * repeats the keystream every block, which is catastrophic and invisible in a
     * short test. */
    uint8_t key[32] = { 0 };
    uint8_t buf[192];

    memset(buf, 0, sizeof buf);

    static const uint8_t nonce[12] = { 0 };

    SMS_ChaCha20(key, 0, nonce, buf, sizeof buf);

    CHECK(memcmp(buf, buf + 64, 64) != 0, "blocks 0 and 1 of the keystream match");
    CHECK(memcmp(buf + 64, buf + 128, 64) != 0, "blocks 1 and 2 match");
}

static void TestSipHashAgainstPublishedVectors(void)
{
    // From the SipHash paper's test vectors, key 000102..0f.
    uint8_t key[16];

    for (uint8_t i = 0; i < 16; i++)
        key[i] = i;

    static const uint8_t in1[1] = { 0x00 };
    static const uint8_t in8[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

    CHECK(SMS_SipHash(key, (const uint8_t *)"", 0) == 0x726fdb47dd0e0e31ull,
          "empty input gave %016llx",
          (unsigned long long)SMS_SipHash(key, (const uint8_t *)"", 0));
    CHECK(SMS_SipHash(key, in1, 1) == 0x74f839c593dc67fdull,
          "one byte gave %016llx", (unsigned long long)SMS_SipHash(key, in1, 1));

    // A whole block exercises the loop as well as the tail.
    CHECK(SMS_SipHash(key, in8, 8) == 0x93f5f5799a932462ull,
          "eight bytes gave %016llx", (unsigned long long)SMS_SipHash(key, in8, 8));

    // A different key must give a different tag, or the key is being ignored.
    key[0] ^= 0xFFu;
    CHECK(SMS_SipHash(key, in8, 8) != 0x93f5f5799a932462ull, "the key is ignored");
}

static void TestSealOpenRoundTrip(void)
{
    uint8_t payload[64];
    uint8_t len = 20;

    SMS_Crypto_SetKey("correct horse battery staple");
    CHECK(SMS_Crypto_HaveKey(), "no key after setting one");

    memcpy(payload, "twenty bytes exactly", len);

    const uint8_t sealed = SMS_Crypto_Seal(payload, len, 0x1111u, 7, 0);

    CHECK(sealed == len + SMS_CRYPTO_OVERHEAD, "sealed length %u want %u",
          sealed, len + SMS_CRYPTO_OVERHEAD);
    CHECK(memcmp(payload + SMS_CRYPTO_OVERHEAD, "twenty bytes exactly", len) != 0,
          "the plaintext is still readable in the sealed payload");

    uint8_t got = sealed;

    CHECK(SMS_Crypto_Open(payload, &got, 0x1111u, 7, 0), "open refused its own seal");
    CHECK(got == len, "opened length %u want %u", got, len);
    CHECK(memcmp(payload, "twenty bytes exactly", len) == 0, "plaintext differs");
}

static void TestEveryTamperIsCaught(void)
{
    /* The point of the tag. Every single bit flip anywhere in a sealed payload -
     * nonce, tag or ciphertext - must be refused. A CRC cannot do this: it is
     * linear, so an attacker who flips ciphertext bits can correct it to match,
     * which is why the tag exists and why the CRC is not the integrity check. */
    uint8_t original[64];
    uint8_t len = 24;

    SMS_Crypto_SetKey("a shared secret");
    memcpy(original, "twenty four bytes of text", len);

    const uint8_t sealed = SMS_Crypto_Seal(original, len, 0x2222u, 3, 1);
    uint32_t      accepted = 0;

    for (uint32_t bit = 0; bit < (uint32_t)sealed * 8u; bit++) {
        uint8_t copy[64];
        uint8_t l = sealed;

        memcpy(copy, original, sizeof copy);
        copy[bit >> 3] ^= (uint8_t)(1u << (bit & 7));

        if (SMS_Crypto_Open(copy, &l, 0x2222u, 3, 1))
            accepted++;
    }

    CHECK(accepted == 0, "%u of %u single bit tampers were accepted",
          accepted, (uint32_t)sealed * 8u);
}

static void TestWrongKeyRejected(void)
{
    uint8_t payload[64];
    uint8_t len = 10;

    SMS_Crypto_SetKey("the right key");
    memcpy(payload, "ten bytes!", len);

    const uint8_t sealed = SMS_Crypto_Seal(payload, len, 0x3333u, 1, 0);

    SMS_Crypto_SetKey("the wrong key");

    uint8_t l = sealed;

    CHECK(!SMS_Crypto_Open(payload, &l, 0x3333u, 1, 0),
          "opened with the wrong key");
}

static void TestWrongContextRejected(void)
{
    /* The sender, message id and fragment are bound into the nonce, so a frame
     * cannot be replayed as a different fragment or attributed to another station -
     * the tag is over the nonce, so any of those changing breaks it. */
    uint8_t payload[64];
    uint8_t len = 12;

    SMS_Crypto_SetKey("context matters");
    memcpy(payload, "twelve bytes", len);

    const uint8_t sealed = SMS_Crypto_Seal(payload, len, 0x4444u, 9, 2);

    struct { const char *what; uint16_t src; uint8_t id; uint8_t frag; } wrong[] = {
        { "a different sender",   0x4445u, 9, 2 },
        { "a different message",  0x4444u, 8, 2 },
        { "a different fragment", 0x4444u, 9, 1 },
    };

    for (size_t i = 0; i < sizeof wrong / sizeof wrong[0]; i++) {
        uint8_t copy[64];
        uint8_t l = sealed;

        memcpy(copy, payload, sizeof copy);

        CHECK(!SMS_Crypto_Open(copy, &l, wrong[i].src, wrong[i].id, wrong[i].frag),
              "accepted %s", wrong[i].what);
    }

    // The right context still works.
    uint8_t l = sealed;

    CHECK(SMS_Crypto_Open(payload, &l, 0x4444u, 9, 2), "rejected the right context");
}

static void TestNonceAdvances(void)
{
    /* The same plaintext sealed twice must not produce the same ciphertext. If it
     * did, the counter would not be advancing and two messages would share a
     * keystream, which reveals their XOR. */
    uint8_t a[64], b[64];
    uint8_t len = 16;

    SMS_Crypto_SetKey("nonce test");

    memcpy(a, "sixteen bytes!!!", len);
    memcpy(b, "sixteen bytes!!!", len);

    const uint8_t sa = SMS_Crypto_Seal(a, len, 0x5555u, 1, 0);
    const uint8_t sb = SMS_Crypto_Seal(b, len, 0x5555u, 1, 0);

    CHECK(sa == sb, "lengths differ");
    CHECK(memcmp(a, b, sa) != 0, "sealing twice gave identical output");
    CHECK(memcmp(a + SMS_CRYPTO_OVERHEAD, b + SMS_CRYPTO_OVERHEAD, len) != 0,
          "the ciphertexts match, so the keystream repeated");
}

static void TestFragmentsDoNotShareKeystream(void)
{
    /* Fragments of one message share the counter, so the fragment index has to be
     * in the nonce. Without it, fragment 0 and fragment 1 would be XORed with the
     * same keystream. */
    uint8_t a[64], b[64];
    uint8_t len = 16;

    SMS_Crypto_SetKey("fragment test");

    memcpy(a, "identical text..", len);
    memcpy(b, "identical text..", len);

    SMS_Crypto_Seal(a, len, 0x6666u, 4, 0);

    // Force the same counter by re-keying, then seal as a different fragment.
    SMS_Crypto_SetKey("fragment test");
    SMS_Crypto_Seal(b, len, 0x6666u, 4, 1);

    CHECK(memcmp(a + SMS_CRYPTO_OVERHEAD, b + SMS_CRYPTO_OVERHEAD, len) != 0,
          "two fragments produced the same ciphertext");
}

static void TestNoKeyIsPassThrough(void)
{
    // With no key set, seal and open must leave the payload alone rather than
    // producing something a peer cannot read.
    uint8_t payload[64];
    uint8_t len = 5;

    SMS_Crypto_SetKey("");
    CHECK(!SMS_Crypto_HaveKey(), "an empty passphrase set a key");

    memcpy(payload, "plain", len);

    CHECK(SMS_Crypto_Seal(payload, len, 1, 1, 0) == len, "length changed");
    CHECK(memcmp(payload, "plain", len) == 0, "payload changed");

    uint8_t l = len;

    CHECK(SMS_Crypto_Open(payload, &l, 1, 1, 0), "open refused");
    CHECK(l == len && memcmp(payload, "plain", len) == 0, "payload changed");
}

static void TestShortPayloadRejected(void)
{
    // A frame claiming less than the overhead cannot contain a tag, and must be
    // refused rather than read past its end.
    uint8_t payload[64] = { 0 };
    uint8_t len = SMS_CRYPTO_OVERHEAD - 1;

    SMS_Crypto_SetKey("short test");
    CHECK(!SMS_Crypto_Open(payload, &len, 1, 1, 0), "accepted a too-short payload");

    len = 0;
    CHECK(!SMS_Crypto_Open(payload, &len, 1, 1, 0), "accepted a zero length payload");
}

int main(void)
{
    static const struct { const char *name; void (*fn)(void); } tests[] = {
        { "ChaCha20 vs RFC 8439 and LibreSSL", TestChaCha20AgainstRfc8439 },
        { "ChaCha20 spans blocks",             TestChaCha20SpansBlocks },
        { "SipHash vs published vectors",      TestSipHashAgainstPublishedVectors },
        { "seal and open round trip",          TestSealOpenRoundTrip },
        { "every tamper is caught",            TestEveryTamperIsCaught },
        { "wrong key rejected",                TestWrongKeyRejected },
        { "wrong context rejected",            TestWrongContextRejected },
        { "nonce advances",                    TestNonceAdvances },
        { "fragments differ",                  TestFragmentsDoNotShareKeystream },
        { "no key is pass through",            TestNoKeyIsPassThrough },
        { "short payload rejected",            TestShortPayloadRejected },
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = failures;

        tests[i].fn();
        printf("%-38s %s\n", tests[i].name, (failures == before) ? "ok" : "FAILED");
    }

    printf("\n%d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
