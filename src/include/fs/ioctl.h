#ifndef _IOCTL_H
#define _IOCTL_H

#define TCGETS		0x5401    // tcgetattr(fd, argp)
#define TCSETS		0x5402    // tcsetattr(fd, TCSANOW, argp)
#define TCSETSW		0x5403    // tcsetattr(fd, TCSADRAIN, argp)
#define TCSETSF		0x5404    // tcsetattr(fd, TCSAFLUSH, argp)
#define TCGETA		0x5405
#define TCSETA		0x5406
#define TCSETAW		0x5407
#define TCSETAF		0x5408
#define TCFLSH		0x540B
#define TIOCSCTTY	0x540E    // Make the given terminal the controlling terminal of the calling process
#define TIOCGPGRP	0x540F    // *argp = tcgetpgrp(fd)
#define TIOCSPGRP	0x5410    // tcsetpgrp(fd, *argp)
#define TIOCSTI		0x5412    // Insert the given byte in the input queue.
#define TIOCGWINSZ	0x5413
#define TIOCSWINSZ	0x5414
#define TIOCNOTTY	0x5422
#define TIOCGSID	0x5429    // *argp = tcgetsid(fd)

#endif
