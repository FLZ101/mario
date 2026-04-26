#include <ncurses.h>

int main()
{
	char first[16];
	char last[16];

	initscr();
	addstr("What is your first name? ");
	refresh();
	getnstr(first,15);

	addstr("What is your last name? ");
	refresh();
	getnstr(last,15);

	printw("Pleased to meet you, %s %s!",first,last);
	refresh();
	getch();

	endwin();
	return 0;
}

