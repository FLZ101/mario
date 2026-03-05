#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <unistd.h>
#include <fcntl.h>

int fputc(int c, FILE *stream)
{
	unsigned char buf = (unsigned char ) c;
	size_t n = fwrite(&buf, 1, 1, stream);
	if (!n)
		return EOF;
	return n;
}

int putchar(int c)
{
	return putc(c, stdout);
}

int fputs(const char *s, FILE *stream)
{
	int c;
	while ((c = *(s++))) {
		int n = fputc(c, stream);
		if (EOF == n)
			return EOF;
	}
	return 0;
}

int puts(const char *s)
{
	int n = fputs(s, stdout);
	if (EOF == n)
		return EOF;

	n = fputc('\n', stdout);
	if (EOF == n)
		return EOF;
	return 0;
}

_syscall2(int,rename,const char *,oldpath, const char *,newpath)

static FILE init_file = {
	.fd = 0, .cache = { 0 }, .p = 0, .p_end = 0,
	.work_mode = READ_ONLY, .buffer_mode = UNBUFFERED
};

static FILE stdin_file = {
	.fd = 0, .cache = { 0 }, .p = 0, .p_end = 0,
	.work_mode = READ_ONLY, .buffer_mode = UNBUFFERED
};
static FILE stdout_file = {
	.fd = 1, .cache = { 0 }, .p = 0, .p_end = 0,
	.work_mode = WRITE_ONLY, .buffer_mode = UNBUFFERED
};
static FILE stderr_file = {
	.fd = 2, .cache = { 0 }, .p = 0, .p_end = 0,
	.work_mode = WRITE_ONLY, .buffer_mode = UNBUFFERED
};

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

FILE *fopen(const char *pathname, const char *mode)
{
	int flags = 0;
	enum FileWorkMode work_mode;

	if (strcmp(mode, "r") == 0 || strcmp(mode, "rb") == 0) {
		flags = O_RDONLY;
		work_mode = READ_ONLY;
	} else if (strcmp(mode, "w") == 0 || strcmp(mode, "wb") == 0) {
		flags = O_WRONLY | O_CREAT | O_TRUNC;
		work_mode = WRITE_ONLY;
	} else if (strcmp(mode, "a") == 0 || strcmp(mode, "ab") == 0) {
		flags = O_WRONLY | O_CREAT | O_APPEND;
		work_mode = WRITE_ONLY;
	} else if (strcmp(mode, "r+") == 0 || strcmp(mode, "rb+") == 0) {
		flags = O_RDWR;
		work_mode = READ;
	} else if (strcmp(mode, "w+") == 0 || strcmp(mode, "wb+") == 0) {
		flags = O_RDWR | O_CREAT | O_TRUNC;
		work_mode = WRITE;
	} else if (strcmp(mode, "a+") == 0 || strcmp(mode, "ab+") == 0) {
		flags = O_RDWR | O_CREAT | O_APPEND;
		work_mode = WRITE;
	} else {
		errno = EINVAL;
		return NULL;
	}

	int fd = open(pathname, flags);
	if (fd == -1) {
		return NULL;
	}

	FILE *f = (FILE *) malloc(sizeof(FILE));
	if (!f) {
		close(fd);
		return NULL;
	}
	*f = init_file;

	switch (work_mode) {
	case WRITE:
	case WRITE_ONLY:
		f->p = 0;
		f->p_end = FILE_CACHE_SIZE;
		break;
	default:
		;
	}

	f->fd = fd;
	f->work_mode = work_mode;
	if (fd > 2)
		f->buffer_mode = FULLY_BUFFERED;
	return f;
}

int fclose(FILE *stream) {
	if (!stream) return EOF;

	int ret = 0;
	if (EOF == fflush(stream))
		ret = EOF;

	if (close(stream->fd) == -1)
		ret = EOF;

	free(stream);
	return ret;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if (!stream)
		return 0;

	if (stream->work_mode == WRITE_ONLY) {
		errno = EBADF;
		return 0;
	}

	if (stream->work_mode == WRITE) {
		fflush(stream);
		stream->work_mode = READ;
	}

	size_t left = size * nmemb;
	size_t n = 0;
	if (stream->buffer_mode == UNBUFFERED) {
		while (n < left) {
			int n_read = read(stream->fd, ptr + n, left - n);
			if (-1 == n_read || 0 == n_read)
				break;
			n += n_read;
		}
		goto done;
	}

	assert(stream->buffer_mode == FULLY_BUFFERED);
	while (left > 0) {
		// if cache is empty
		if (stream->p >= stream->p_end) {
			int n_read = read(stream->fd, stream->cache, FILE_CACHE_SIZE);
			if (n_read == -1) // error
				break;
			if (n_read == 0) // EOF
				break;
			stream->p = 0;
			stream->p_end = n_read;
		}

		while (left > 0 && stream->p < stream->p_end) {
			*(char *)ptr++ = stream->cache[stream->p++];
			--left;
			++n;
		}
	}
done:
	return (size_t) n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if (!stream)
		return 0;

	if (stream->work_mode == READ_ONLY) {
		errno = EBADF;
		return 0;
	}

	if (stream->work_mode == READ) {
		fflush(stream);
		stream->work_mode = WRITE;
	}

	size_t left = size * nmemb;
	size_t n = 0;

	if (stream->buffer_mode == UNBUFFERED) {
		while (n < left) {
			int n_write = write(stream->fd, ptr + n, left - n);
			if (-1 == n_write || 0 == n_write)
				break;
			n += n_write;
		}
		goto done;
	}

	assert(stream->buffer_mode == FULLY_BUFFERED);
	while (left > 0) {
		// write to cache first
		while (left > 0 && stream->p < stream->p_end) {
			stream->cache[stream->p++] = * (char *) ptr++;
			--left;
			++n;
		}

		// if cache is full
		if (stream->p == stream->p_end) {
			int i = 0;
			while (i < stream->p) {
				int n_write = write(stream->fd, stream->cache + i, stream->p - i);
				if (-1 == n_write || 0 == n_write) {
					goto done;
				}
				i += n_write;
			}
			stream->p = 0;
			stream->p_end = FILE_CACHE_SIZE;
		}
	}
done:
	return (size_t) n / size;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
	// 1. Validation
	if (!lineptr || !n || !stream) {
		errno = EINVAL;
		return -1;
	}

	// 2. Initialize buffer if needed
	if (*lineptr == NULL || *n == 0) {
		*n = 128; // Default initial size
		*lineptr = malloc(*n);
		if (*lineptr == NULL) {
			return -1; // Memory allocation failed
		}
	}

	size_t len = 0;
	char c;

	// 3. Read loop
	while (1) {
		// Read one character
		if (fread(&c, 1, 1, stream) == 0) {
			// EOF or Error
			if (len == 0) {
				return EOF; // No data read, true EOF
			}
			break; // Data was read, but now EOF. Finish the line.
		}

		// 4. Check if we need to resize the buffer
		// We need space for the new char + the null terminator '\0'
		if (len + 1 >= *n) {
			size_t new_size = *n * 2; // Double the size
			char *new_ptr = realloc(*lineptr, new_size);

			if (new_ptr == NULL) {
				// Reallocation failed.
				// We stop here, null-terminate what we have, and return.
				(*lineptr)[len] = '\0';
				return (ssize_t)len;
			}

			*lineptr = new_ptr;
			*n = new_size;
		}

		// 5. Store character
		(*lineptr)[len++] = c;

		// 6. Stop if newline
		if (c == '\n') {
			break;
		}
	}

	// 7. Null-terminate the string
	(*lineptr)[len] = '\0';

	return (ssize_t)len;
}

// Upon successful completion 0 is returned. Otherwise, EOF is returned and errno is set to indicate the error.
int fflush(FILE *stream)
{
	switch (stream->work_mode) {
	case READ:
	case READ_ONLY: {
		if (stream->p < stream->p_end) {
			if (-1 == lseek(stream->fd, stream->p - stream->p_end, SEEK_CUR))
				return EOF;
			stream->p = 0;
			stream->p_end = 0;
		}
		return 0;
	}
	case WRITE:
	case WRITE_ONLY: {
		int i = 0;
		while (i < stream->p) {
			int n_write = write(stream->fd, stream->cache + i, stream->p - i);
			if (-1 == n_write || 0 == n_write)
				return EOF;
			i += n_write;
		}
		stream->p = 0;
		stream->p_end = FILE_CACHE_SIZE;
		return 0;
	}
	default:
		;
	}
	return 0;
}

// Upon successful completion, returns 0.
// Otherwise, -1 is returned and errno is set to indicate the error
int fseek(FILE *stream, long offset, int whence)
{
	if (EOF == fflush(stream))
		return -1;
	if (-1 == lseek(stream->fd, offset, whence))
		return -1;
	return 0;
}

// Upon successful completion, returns the current offset.
// Otherwise, -1 is returned and errno is set to indicate the error
long ftell(FILE *stream)
{
	if (EOF == fflush(stream))
		return -1;
	return lseek(stream->fd, 0, SEEK_CUR);
}

void rewind(FILE *stream)
{
	(void) fseek(stream, 0L, SEEK_SET);
}
