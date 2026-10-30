#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <lib/string.h>

/*
 * The ext2/ext3 htree uses the same half-MD4 filename hash as Linux
 * fs/ext4/hash.c.  The transform is MD4 with rounds 2 and 3 truncated to a
 * single application of the G and H functions; only buf[1] is returned as
 * the directory hash, with the low bit cleared for use as an index key.
 */
#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))

#define K1 0
#define K2 0x5a827999U
#define K3 0x6ed9eba1U

static uint32_t rol32(uint32_t value, int bits)
{
	return (value << bits) | (value >> (32 - bits));
}

#define ROUND(f, a, b, c, d, x, s)				\
	do {							\
		(a) += f((b), (c), (d)) + (x);			\
		(a) = rol32((a), (s));				\
	} while (0)

static void half_md4_transform(uint32_t buf[4], const uint32_t in[8])
{
	uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];

	ROUND(F, a, b, c, d, in[0] + K1, 3);
	ROUND(F, d, a, b, c, in[1] + K1, 7);
	ROUND(F, c, d, a, b, in[2] + K1, 11);
	ROUND(F, b, c, d, a, in[3] + K1, 19);
	ROUND(F, a, b, c, d, in[4] + K1, 3);
	ROUND(F, d, a, b, c, in[5] + K1, 7);
	ROUND(F, c, d, a, b, in[6] + K1, 11);
	ROUND(F, b, c, d, a, in[7] + K1, 19);

	ROUND(G, a, b, c, d, in[1] + K2, 3);
	ROUND(G, d, a, b, c, in[3] + K2, 5);
	ROUND(G, c, d, a, b, in[5] + K2, 9);
	ROUND(G, b, c, d, a, in[7] + K2, 13);
	ROUND(G, a, b, c, d, in[0] + K2, 3);
	ROUND(G, d, a, b, c, in[2] + K2, 5);
	ROUND(G, c, d, a, b, in[4] + K2, 9);
	ROUND(G, b, c, d, a, in[6] + K2, 13);

	ROUND(H, a, b, c, d, in[3] + K3, 3);
	ROUND(H, d, a, b, c, in[7] + K3, 9);
	ROUND(H, c, d, a, b, in[2] + K3, 11);
	ROUND(H, b, c, d, a, in[6] + K3, 15);
	ROUND(H, a, b, c, d, in[1] + K3, 3);
	ROUND(H, d, a, b, c, in[5] + K3, 9);
	ROUND(H, c, d, a, b, in[0] + K3, 11);
	ROUND(H, b, c, d, a, in[4] + K3, 15);

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}

#undef ROUND
#undef F
#undef G
#undef H
#undef K1
#undef K2
#undef K3

static void str2hashbuf(const char *name, int len, uint32_t *buf, int num)
{
	uint32_t pad = (uint32_t)len | ((uint32_t)len << 8);
	uint32_t val;
	int i;

	pad |= pad << 16;
	val = pad;
	if (len > num * 4)
		len = num * 4;

	for (i = 0; i < len; i++) {
		int c = (signed char)name[i];

		val = (uint32_t)c + (val << 8);
		if ((i % 4) == 3) {
			*buf++ = val;
			val = pad;
			num--;
		}
	}
	if (--num >= 0)
		*buf++ = val;
	while (--num >= 0)
		*buf++ = pad;
}

int ext2_dirhash(struct super_block *sb, const char *name, int len,
		 uint32_t *hash)
{
	uint32_t buf[4];
	uint32_t in[8];
	int i;

	if (!sb || !name || len <= 0 ||
	    EXT2_SB(sb).hash_version != EXT2_HASH_HALF_MD4)
		return -ENOTSUP;

	buf[0] = 0x67452301U;
	buf[1] = 0xefcdab89U;
	buf[2] = 0x98badcfeU;
	buf[3] = 0x10325476U;
	for (i = 0; i < 4; i++) {
		if (EXT2_SB(sb).hash_seed[i])
			break;
	}
	if (i < 4)
		memcpy(buf, EXT2_SB(sb).hash_seed, sizeof(buf));

	while (len > 0) {
		str2hashbuf(name, len, in, 8);
		half_md4_transform(buf, in);
		len -= 32;
		name += 32;
	}

	*hash = buf[1] & ~1U;
	return 0;
}
