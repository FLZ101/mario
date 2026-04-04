#ifndef _LIMITS_H
#define _LIMITS_H

#define NR_OPEN		 256
#define NAME_MAX	 255	/* # chars in a file name */
#define PATH_MAX	2048	/* # chars in a path name */
                            /* Make sure size of a dirent is less than PAGE_SIZE */
#define ARG_MAX       131072	/* # bytes of args + environ for exec() */

#endif
