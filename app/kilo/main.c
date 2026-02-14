#include <unistd.h>

/*
 * https://github.com/snaptoken/kilo-src/
 * https://viewsourcecode.org/snaptoken/kilo/
 */

int main() {
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q');
  return 0;
}
