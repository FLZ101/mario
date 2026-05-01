#include <ncurses.h>

int main()
{
	int ch;

	initscr();

	keypad(stdscr,TRUE);
	do
	{
		ch = getch();
		switch(ch)
		{
			case KEY_DOWN:
				addstr("Down\n");
				break;
			case KEY_UP:
				addstr("Up\n");
				break;
			case KEY_LEFT:
				addstr("Left\n");
				break;
			case KEY_RIGHT:
				addstr("Right\n");
				break;
            case KEY_HOME:
                addstr("Home\n");
                break;
            case KEY_END:
                addstr("End\n");
                break;
            case KEY_NPAGE:
                addstr("Page Down\n");
                break;
            case KEY_PPAGE:
                addstr("Page Up\n");
                break;
			case KEY_BACKSPACE:
				addstr("Backspace\n");
				break;
			case KEY_IC:
				addstr("Insert\n");
				break;
			case KEY_DC:
				addstr("Delete\n");
				break;
            default:
                break;
            }
		refresh();
	} while(ch != '\n');

	endwin();
	return 0;
}
