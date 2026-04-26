#include <app/util.h>

int main(int argc, char *argv[]) {
    if (argc > 1) {
        ListDir(argv[1]);
    } else {
        ListDir(".");
    }
    return 0;
}
