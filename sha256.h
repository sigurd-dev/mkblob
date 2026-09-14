/*
 * sha256.h - minimal, self-contained SHA-256 (FIPS 180-4) plus a small
 * salted/iterated key-derivation helper for password verification.
 *
 * Header-only and static so it can simply be #included by any translation
 * unit that needs it (mkblob.c, mkblobexec.c) without adding a new object
 * file to link.
 *
 * This is a straightforward, from-the-spec SHA-256 implementation, not a
 * hardened/side-channel-resistant crypto library. derive_key() below is a
 * simple iterated-hash construction (not a vetted KDF like PBKDF2/Argon2)
 * chosen to avoid any external crypto dependency in a statically linked
 * tool; it is meant to raise the cost of offline brute-forcing a weak
 * password, not to provide strong cryptographic guarantees.
 */

#ifndef MKBLOB_SHA256_H
#define MKBLOB_SHA256_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    unsigned char buffer[64];
    size_t buflen;
} sha256_ctx;

#define SHA256_ROTR(a, b) (((a) >> (b)) | ((a) << (32 - (b))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_EP1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_SIG0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_transform(sha256_ctx *ctx, const unsigned char data[64])
{
    uint32_t m[64];
    size_t i, j;

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | (uint32_t)data[j + 3];
    for (; i < 64; ++i)
        m[i] = SHA256_SIG1(m[i - 2]) + m[i - 7] + SHA256_SIG0(m[i - 15]) +
               m[i - 16];

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2],
             d = ctx->state[3], e = ctx->state[4], f = ctx->state[5],
             g = ctx->state[6], h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        uint32_t t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) + sha256_k[i] + m[i];
        uint32_t t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx)
{
    ctx->buflen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(sha256_ctx *ctx, const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        ctx->buffer[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, unsigned char digest[32])
{
    size_t i = ctx->buflen;

    if (ctx->buflen < 56) {
        ctx->buffer[i++] = 0x80;
        while (i < 56)
            ctx->buffer[i++] = 0x00;
    } else {
        ctx->buffer[i++] = 0x80;
        while (i < 64)
            ctx->buffer[i++] = 0x00;
        sha256_transform(ctx, ctx->buffer);
        memset(ctx->buffer, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->buflen * 8;
    for (i = 0; i < 8; ++i)
        ctx->buffer[63 - i] = (unsigned char)(ctx->bitlen >> (8 * i));

    sha256_transform(ctx, ctx->buffer);

    for (i = 0; i < 4; ++i) {
        for (int j = 0; j < 8; ++j)
            digest[j * 4 + i] = (unsigned char)(ctx->state[j] >> (24 - i * 8));
    }
}

static void sha256_once(const unsigned char *data, size_t len,
                         unsigned char digest[32])
{
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

/* digest0 = SHA256(salt || password)
 * digest_i = SHA256(salt || digest_{i-1}) for iterations-1 more rounds. */
static void derive_key(const char *password, const unsigned char salt[16],
                        uint32_t iterations, unsigned char out[32])
{
    unsigned char buf[16 + 32];

    memcpy(buf, salt, 16);
    {
        sha256_ctx ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, salt, 16);
        sha256_update(&ctx, (const unsigned char *)password, strlen(password));
        sha256_final(&ctx, out);
    }
    for (uint32_t i = 1; i < iterations; i++) {
        memcpy(buf, salt, 16);
        memcpy(buf + 16, out, 32);
        sha256_once(buf, sizeof buf, out);
    }
}

/* Folds additional binding factors (an expiry deadline, a required MAC
 * address, a required machine UUID) into a base key from derive_key()
 * above. All-zero expire_ts/mac_bytes/uuid_bytes mean "no restriction",
 * matching whatever value was used when the payload was encrypted -- see
 * mkblob.c's encrypt_payload() and mkblobexec.c's extract_embedded_blob(). */
static void mix_key(const unsigned char base_key[32],
                     const unsigned char expire_ts[8],
                     const unsigned char mac_bytes[6],
                     const unsigned char uuid_bytes[16],
                     unsigned char out[32])
{
    unsigned char buf[32 + 8 + 6 + 16];
    memcpy(buf, base_key, 32);
    memcpy(buf + 32, expire_ts, 8);
    memcpy(buf + 40, mac_bytes, 6);
    memcpy(buf + 46, uuid_bytes, 16);
    sha256_once(buf, sizeof buf, out);
}

/* Self-contained stream cipher: keystream block i = SHA256(key || salt ||
 * counter_i), concatenated and XORed against data (encrypt and decrypt
 * are the same operation). Not a vetted cipher like AES-CTR, but adequate
 * for the stated goal (keep the embedded payload unreadable without the
 * derived key), consistent with the no-external-crypto-library approach
 * used for derive_key() above. */
static void keystream_xor(unsigned char *data, size_t len,
                           const unsigned char key[32],
                           const unsigned char salt[16])
{
    unsigned char seed[32 + 16 + 4];
    memcpy(seed, key, 32);
    memcpy(seed + 32, salt, 16);

    size_t off = 0;
    uint32_t counter = 0;
    while (off < len) {
        seed[48] = (unsigned char)(counter >> 24);
        seed[49] = (unsigned char)(counter >> 16);
        seed[50] = (unsigned char)(counter >> 8);
        seed[51] = (unsigned char)counter;

        unsigned char block[32];
        sha256_once(seed, sizeof seed, block);

        size_t n = len - off;
        if (n > sizeof block)
            n = sizeof block;
        for (size_t i = 0; i < n; i++)
            data[off + i] ^= block[i];

        off += n;
        counter++;
    }
}

#endif /* MKBLOB_SHA256_H */
