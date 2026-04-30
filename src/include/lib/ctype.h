#ifndef _CTYPE_H
#define _CTYPE_H

static inline int islower(int ch) { return 'a' <= ch && ch <= 'z'; }

static inline int isupper(int ch) { return 'A' <= ch && ch <= 'Z'; }

static inline int isalpha(int ch) { return islower(ch) || isupper(ch); }

static inline int isdigit(int ch) { return '0' <= ch && ch <= '9'; }

static inline int isalnum(int ch) { return isalpha(ch) || isdigit(ch); }

static inline int isxdigit(int ch) {
    return isdigit(ch) || ('a' <= ch && ch <= 'f') || ('A' <= ch && ch <= 'F');
}

static inline int isspace(int ch) {
    switch (ch) {
    default:
        return 0;
    case ' ':
    case '\f':
    case '\n':
    case '\r':
    case '\t':
    case '\v':
        return 1;
    }
}

static inline int iscntrl(int ch) { return 0x00 <= ch && ch <= 0x7f; }

static inline int ispunct(int ch) { return '!' <= ch && ch <= '~'; }

static inline int isgraph(int ch) { return isalnum(ch) || ispunct(ch); }

static inline int isprint(int ch) { return isgraph(ch) || ch == ' '; }

#endif
