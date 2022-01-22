#ifndef _KBD_H
#define _KBD_H

struct v_key {
	unsigned int v_flags;
	unsigned int v_key;
};

struct kbd {
	struct v_key key;
};

void kbd_init(struct kbd *k);

#endif	/* _KBD_H */