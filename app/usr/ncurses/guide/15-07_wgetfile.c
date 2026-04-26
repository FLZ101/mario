#include <ncurses.h>

int main()
{
	FILE *wfile;
	WINDOW *win;

	initscr();
	start_color();
	init_pair(1,COLOR_WHITE,COLOR_RED);
	refresh();

/* open file */
	wfile = fopen("window.win","r");
	if( wfile==NULL )
	{
		endwin();
		puts("Error reading file");
		return(1);
	}

/* read window's data */
	win = getwin(wfile);
	if( win==NULL )
		addstr("Unable to read file\n");
	else
		wrefresh(win);
	fclose(wfile);
	getch();

	endwin();
	return 0;
}
