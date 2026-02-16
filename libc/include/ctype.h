#ifndef _CTYPE_H
#define _CTYPE_H

static inline int iscntrl(int ch)
{
    return (0 <= ch && ch <= 31) || (ch == 127);
}

#endif
