#ifndef _IOCTL_H
#define _IOCTL_H

#define _IOR(a, b) ((((a) & 0xffff) << 16) + ((b) & 0xffff))

#define TCGETS      _IOR('t', 0)    // tcgetattr(fd, argp)
#define TCSETSF     _IOR('t', 1)    // tcsetattr(fd, TCSAFLUSH, argp)
#define TCSETSW     _IOR('t', 2)    // tcsetattr(fd, TCSADRAIN, argp)
#define TCSETS      _IOR('t', 3)    // tcsetattr(fd, TCSANOW, argp)
#define TIOCGWINSZ  _IOR('t', 4)
#define TIOCSWINSZ  _IOR('t', 5)
#define TIOCSTI     _IOR('t', 6)    // Insert the given byte in the input queue.
#define TIOCSCTTY   _IOR('t', 7)    // Make the given terminal the controlling terminal of the calling process
#define TIOCNOTTY   _IOR('t', 8)
#define TIOCGPGRP   _IOR('t', 9)    // *argp = tcgetpgrp(fd)
#define TIOCSPGRP   _IOR('t', 10)   // tcsetpgrp(fd, *argp)
#define TIOCGSID    _IOR('t', 11)   // *argp = tcgetsid(fd)

#endif
