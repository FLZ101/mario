#include <ncurses.h>

int main()
{
	MEVENT mort;
	int ch;
	
	initscr();
	noecho();
	keypad(stdscr,TRUE);

	mousemask(ALL_MOUSE_EVENTS,NULL);
	addstr("Click the mouse around the screen\n");
	addstr("Press Enter to stop");
	while(1)
	{
		ch = getch();
		if( ch == KEY_MOUSE )
		{
			getmouse(&mort);
			mvaddch(mort.y,mort.x,'*');		/* new statement */
			refresh();
			continue;
		}
		if( ch == '\n' )
			break;
	}
	
	endwin();
	return(0);
}

