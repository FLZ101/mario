#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "pl_string.h"

#define PRINT_NUMBER(name, type, format, ...)                        \
    int pl_str_print_##name(char *buf, size_t len, type value)        \
    {                                                                \
        int count = snprintf(buf, len, format, __VA_ARGS__);          \
        return count >= 0 && (size_t)count < len ? count : 0;          \
    }

PRINT_NUMBER(hex, unsigned short, "%hx", value)
PRINT_NUMBER(int, int, "%d", value)
PRINT_NUMBER(uint, unsigned int, "%u", value)
PRINT_NUMBER(int64, int64_t, "%" PRId64, value)
PRINT_NUMBER(uint64, uint64_t, "%" PRIu64, value)
PRINT_NUMBER(float, float, "%.*g", FLT_DECIMAL_DIG, (double)value)
PRINT_NUMBER(double, double, "%.*g", DBL_DECIMAL_DIG, value)

static bool copy_number(pl_str str, char *buf, size_t len)
{
    if (!str.len || str.len >= len || isspace(str.buf[0]) ||
        str.buf[0] == '+' || memchr(str.buf, '\0', str.len))
        return false;
    memcpy(buf, str.buf, str.len);
    buf[str.len] = '\0';
    return true;
}

#define PARSE_INT(name, type, wide, parse, radix, min, max, unsign)    \
    bool pl_str_parse_##name(pl_str str, type *out)                   \
    {                                                                \
        char buf[128];                                               \
        char *end;                                                   \
        if (!copy_number(str, buf, sizeof(buf)) ||                    \
            (unsign && buf[0] == '-'))                               \
            return false;                                            \
        errno = 0;                                                   \
        wide value = parse(buf, &end, radix);                         \
        if (errno || *end || value < (wide)(min) ||                   \
            value > (wide)(max))                                     \
            return false;                                            \
        *out = value;                                                \
        return true;                                                 \
    }

PARSE_INT(hex, unsigned short, uintmax_t, strtoumax, 16, 0, USHRT_MAX, true)
PARSE_INT(int, int, intmax_t, strtoimax, 10, INT_MIN, INT_MAX, false)
PARSE_INT(uint, unsigned int, uintmax_t, strtoumax, 10, 0, UINT_MAX, true)
PARSE_INT(int64, int64_t, intmax_t, strtoimax, 10, INT64_MIN, INT64_MAX, false)
PARSE_INT(uint64, uint64_t, uintmax_t, strtoumax, 10, 0, UINT64_MAX, true)

#define PARSE_FLOAT(type, parse)                                     \
    bool pl_str_parse_##type(pl_str str, type *out)                   \
    {                                                                \
        char buf[128];                                               \
        char *end;                                                   \
        if (!copy_number(str, buf, sizeof(buf)))                     \
            return false;                                            \
        errno = 0;                                                   \
        type value = parse(buf, &end);                                \
        if (errno || *end)                                           \
            return false;                                            \
        *out = value;                                                \
        return true;                                                 \
    }

PARSE_FLOAT(float, strtof)
PARSE_FLOAT(double, strtod)
