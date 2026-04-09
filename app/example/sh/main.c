#include <stdio.h>

int main(void)
{
    char *line = NULL;
    size_t n = 100;
    getline(&line, &n, stdin);
    puts(line);
    return 0;
}
