#ifndef _STDDEF_H
#define _STDDEF_H

typedef unsigned int size_t;

#define NULL	((void *)0)

#define offsetof(t,m)	((size_t)&((t *)0)->m)

#endif /* _STDDEF_H */