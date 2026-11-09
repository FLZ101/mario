#include <assert.h>
#include <stdio.h>

#include <png.h>

int main(void)
{
	/* Link and run against the pinned library instead of only checking headers. */
	assert(png_access_version_number() == PNG_LIBPNG_VER);
	printf("png-decoder: libpng %s\n", PNG_LIBPNG_VER_STRING);
	return 0;
}
