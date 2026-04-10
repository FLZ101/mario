#include <stdio.h>

int main(int argc, char *argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (i > 0)
            putchar(' ');
        printf("%s", argv[i]);
    }
    printf("\n");
    return 0;
}
