#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

// Helper: parse signed decimal integer (%d)
static const char *parse_int(const char *s, int *out)
{
    if (!*s)
        return NULL;

    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    if (!isdigit((unsigned char)*s))
        return NULL;

    int val = 0;
    while (isdigit((unsigned char)*s)) {
        val = val * 10 + (*s - '0');
        s++;
    }

    *out = sign * val;
    return s;
}

// Helper: parse unsigned decimal (%u)
static const char *parse_uint_dec(const char *s, unsigned int *out)
{
    if (!*s || !isdigit((unsigned char)*s))
        return NULL;

    unsigned int val = 0;
    while (isdigit((unsigned char)*s)) {
        val = val * 10 + (*s - '0');
        s++;
    }

    *out = val;
    return s;
}

// Helper: parse unsigned hex (%x) — supports 0x, 0X, or plain
static const char *parse_uint_hex(const char *s, unsigned int *out)
{
    if (!*s)
        return NULL;

    // Optional 0x / 0X
    if (*s == '0') {
        s++;
        if (*s == 'x' || *s == 'X') {
            s++; // skip x/X
        } else {
            // It was just "0"
            *out = 0;
            return s;
        }
    }

    if (!*s || !isxdigit((unsigned char)*s))
        return NULL;

    unsigned int val = 0;
    while (isxdigit((unsigned char)*s)) {
        if (isdigit(*s)) {
            val = val * 16 + (*s - '0');
        } else if (*s >= 'a' && *s <= 'f') {
            val = val * 16 + (*s - 'a' + 10);
        } else if (*s >= 'A' && *s <= 'F') {
            val = val * 16 + (*s - 'A' + 10);
        }
        s++;
    }

    *out = val;
    return s;
}

int vsscanf(const char *str, const char *format, va_list args)
{
    const char *s = str;
    const char *f = format;
    int assigned = 0;

    while (*f) {
        if (*f == '%') {
            f++; // skip '%'

            // Handle %%
            if (*f == '%') {
                if (*s != '%')
                    goto match_failure;
                s++;
                f++;
                continue;
            }

            char spec = *f++;
            const char *next = NULL;

            switch (spec) {
            case 'd': {
                int *p = va_arg(args, int *);
                // Skip leading whitespace for numeric conversions
                while (*s && isspace((unsigned char)*s))
                    s++;
                if (!*s)
                    goto input_failure;
                next = parse_int(s, p);
                if (!next)
                    goto input_failure;
                s = next;
                assigned++;
                break;
            }
            case 'u': {
                unsigned int *p = va_arg(args, unsigned int *);
                while (*s && isspace((unsigned char)*s))
                    s++;
                if (!*s)
                    goto input_failure;
                next = parse_uint_dec(s, p);
                if (!next)
                    goto input_failure;
                s = next;
                assigned++;
                break;
            }
            case 'x': {
                unsigned int *p = va_arg(args, unsigned int *);
                while (*s && isspace((unsigned char)*s))
                    s++;
                if (!*s)
                    goto input_failure;
                next = parse_uint_hex(s, p);
                if (!next)
                    goto input_failure;
                s = next;
                assigned++;
                break;
            }
            case 'c': {
                // %c does NOT skip whitespace by itself
                if (!*s)
                    goto input_failure;
                char *p = va_arg(args, char *);
                *p = *s++;
                assigned++;
                break;
            }
            default:
                // Unsupported specifier
                goto input_failure;
            }
        }
        else {
            // Handle literal in format string
            if (isspace((unsigned char)*f)) {
                // Whitespace in format: skip ALL whitespace in input
                while (*s && isspace((unsigned char)*s)) {
                    s++;
                }
                f++; // consume one whitespace from format
            } else {
                // Non-whitespace literal: must match exactly
                if (*s != *f)
                    goto match_failure;
                s++;
                f++;
            }
        }
    }

    return assigned;

match_failure:
    // Format mismatch — return assignments so far (per C standard)
    return assigned;

input_failure:
    // Input exhausted or conversion failed
    return EOF; // standard says return EOF on input failure
}

int sscanf(const char *str, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int ret = vsscanf(str, format, args);
    va_end(args);
    return ret;
}
