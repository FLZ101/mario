#ifndef MARIO_LWIP_STDLIB_H
#define MARIO_LWIP_STDLIB_H

static inline int atoi(const char *str)
{
	unsigned int value = 0;

	while (*str >= '0' && *str <= '9') {
		value = value * 10U + (*str - '0');
		str++;
	}
	return (int)value;
}

#endif
