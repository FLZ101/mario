#include <lib/crc32.h>

/*
 * Standard reflected CRC32 used by GPT partition metadata.
 *
 * The initial value is traditionally 0xffffffff, so this function complements
 * the supplied CRC, processes all input bytes, and complements the result.
 * Calling it as crc32(0, buf, len) therefore gives the usual crc32(buf, len).
 */
uint32_t crc32(uint32_t crc, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t i;

	crc = ~crc;
	for (i = 0; i < len; i++) {
		unsigned int bit;

		crc ^= p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
	}
	return ~crc;
}
