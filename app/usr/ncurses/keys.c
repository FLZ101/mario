#include <ncurses.h>

int main() {
    initscr();
    nonl();
    raw();
    keypad(stdscr, TRUE);
    noecho();

    // Tell ncurses to force 'Meta' interpretation for Alt keys
    meta(stdscr, TRUE);

    mvprintw(0, 0, "Press 'q' to quit.");
    move(2, 0);

    int ch;
    while((ch = getch()) != 'q') {
        move(2, 0);
        clrtoeol();

        const char* name = keyname(ch);
        printw("Input: [%s] | Dec: %d | Hex: 0x%02X", name ? name : "Unknown", ch, ch);
        refresh();
    }

    endwin();
    return 0;
}
