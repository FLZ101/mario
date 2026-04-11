#include <stdio.h>

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (i > 1)
            putchar(' ');
        printf("%s", argv[i]);
    }
    printf("\n");
    return 0;
}
