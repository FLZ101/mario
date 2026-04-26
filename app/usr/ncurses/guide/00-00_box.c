#include <ncurses.h>

int main()
{
	/* start Ncurses */
	initscr();

	box(stdscr,'*','*');
	refresh();
	getch();

	/* clean-up */
	endwin();
	return 0;
}
