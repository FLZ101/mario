#ifndef _CTYPE_H
#define _CTYPE_H

static inline int iscntrl(int ch)
{
    return (0 <= ch && ch <= 31) || (ch == 127);
}

static inline int isdigit(int ch)
{
    return '0' <= ch && ch <= '9';
}

static inline int isxdigit(int ch)
{
   return ('0' <= ch && ch <= '9') || ('a' <= ch && ch <= 'f') || ('A' <= ch && ch <= 'F');
}

static inline int isspace(int ch)
{
    return ch == ' ' || ch == '\f' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '\v';
}

#endif
