/* Authenticated encryption for SMS: ChaCha20 for confidentiality, SipHash-2-4
 * for authenticity.
 *
 * WHY THESE TWO, on this core specifically.
 *
 * ChaCha20 is 32-bit add, xor and rotate with no tables. AES on a Cortex-M0 wants
 * a 256-byte S-box and is both larger and slower here; ChaCha is the better fit
 * for the same security level.
 *
 * SipHash rather than Poly1305, which would be the usual partner, because ARMv6-M
 * has MULS but not UMULL: there is no 32x32->64 multiply on this core, that arrived
 * with Cortex-M3. Poly1305's arithmetic is modulo 2^130-5 and needs those wide
 * products synthesised by hand, which costs both code and cycles. SipHash needs
 * only 64-bit add, rotate and xor - two instructions each on a 32-bit core, and no
 * multiply at all - and its 64-bit output is exactly the tag budget a 64 byte frame
 * can spare.
 *
 * ENCRYPT-THEN-MAC. The tag covers the nonce and the ciphertext, and is checked
 * before anything is decrypted. The frame's CRC is not and cannot be the integrity
 * check: a CRC is linear, so an attacker who flips ciphertext bits can correct it
 * to match. The CRC catches noise; the tag catches people.
 *
 * WHAT THIS DOES NOT GIVE YOU, stated plainly because the gaps matter:
 *
 *   No forward secrecy and no key exchange. The key is a pre-shared passphrase;
 *   X25519 would be 3 to 4kB and does not fit.
 *
 *   The passphrase is stretched only by iterated ChaCha20, which is a speed bump
 *   and not a password-hashing function. There is no room for scrypt or even
 *   PBKDF2 at a useful iteration count, so the passphrase itself has to carry the
 *   entropy.
 *
 *   Nonce uniqueness across reboots is not guaranteed. The counter is seeded from
 *   SysTick and the chip ID at start rather than persisted, because there is no
 *   spare EEPROM handling in the budget. Repeating a nonce with a stream cipher
 *   reveals the XOR of two plaintexts, so this is a real if unlikely weakness:
 *   collisions are birthday-bound on 32 bits.
 *
 * And one legal point, once. Encryption is prohibited on amateur bands in most
 * jurisdictions, which forbid obscuring the meaning of a transmission.
 * Authentication is not - a tag proves who sent something without hiding what it
 * says. ENABLE_SMS_CRYPTO is on by default here at the operator's request; whether
 * that is lawful where you are is your call, not this code's.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#ifndef SMS_CRYPTO_H
#define SMS_CRYPTO_H

#include <stdbool.h>
#include <stdint.h>

// Sealed payload layout: nonce, tag, then ciphertext.
#define SMS_NONCE_BYTES      4
#define SMS_TAG_BYTES        8
#define SMS_CRYPTO_OVERHEAD  (SMS_NONCE_BYTES + SMS_TAG_BYTES)

// --- primitives, exposed so the host test can check them against published
// --- vectors and against openssl rather than only against themselves

/* ChaCha20 as RFC 8439: 32 byte key, 32 bit block counter, 96 bit nonce, XORed
 * over buf in place. */
void SMS_ChaCha20(const uint8_t *key, uint32_t counter, const uint8_t *nonce,
                  uint8_t *buf, uint32_t len);

// SipHash-2-4 with a 16 byte key, returning the 64 bit tag.
uint64_t SMS_SipHash(const uint8_t *key, const uint8_t *data, uint32_t len);

// --- the wrapper the link layer uses --------------------------------------

/* Derives the key from a passphrase. Iterated ChaCha20, which slows a brute force
 * attempt by a factor of a few thousand for about 40ms of key-set time and twenty
 * bytes of code. Not a password hashing function; see the header comment. */
void SMS_Crypto_SetKey(const char *passphrase);

bool SMS_Crypto_HaveKey(void);

/* Seals a payload in place. text_len bytes of plaintext at the start of payload
 * become SMS_CRYPTO_OVERHEAD + text_len bytes of nonce, tag and ciphertext, so the
 * buffer must have room. Returns the new length.
 *
 * src, msg_id and frag go into the ChaCha nonce alongside the counter, so the same
 * counter value on different fragments of the same message still gives distinct
 * keystreams.
 */
uint8_t SMS_Crypto_Seal(uint8_t *payload, uint8_t text_len,
                        uint16_t src, uint8_t msg_id, uint8_t frag);

/* Opens a payload in place, writing the plaintext length. Returns false if the tag
 * does not verify, in which case the payload is left untouched and the frame must
 * be discarded - not merely reported as corrupt, since a bad tag means someone
 * constructed it.
 */
bool SMS_Crypto_Open(uint8_t *payload, uint8_t *len,
                     uint16_t src, uint8_t msg_id, uint8_t frag);

#endif
