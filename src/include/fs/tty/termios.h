#ifndef _TERMIOS_H
#define _TERMIOS_H

typedef unsigned char cc_t;
typedef unsigned int tcflag_t;

#define NCCS 19

struct termios {
    tcflag_t c_iflag;	/* input mode flags */
    tcflag_t c_oflag;	/* output mode flags */
    tcflag_t c_cflag;	/* control mode flags */
    tcflag_t c_lflag;	/* local mode flags */
    cc_t c_cc[NCCS];	/* control characters */
};

/* c_iflag bits */
#define IGNBRK	0000001
#define BRKINT	0000002
#define IGNPAR	0000004
#define PARMRK	0000010
#define INPCK	0000020
#define ISTRIP	0000040
#define INLCR	0000100 // Translate NL to CR on input.
#define IGNCR	0000200 // Ignore carriage return on input.
#define ICRNL	0000400 // Translate carriage return to newline on input (unless IGNCR is set).
#define IUCLC	0001000
#define IXON	0002000
#define IXANY	0004000
#define IXOFF	0010000
#define IMAXBEL	0020000

/* c_oflag bits */
#define OPOST	0000001
#define OLCUC	0000002
#define ONLCR	0000004 // Map NL to CR-NL on output.
#define OCRNL	0000010 // Map CR to NL on output.
#define ONOCR	0000020 // Don't output CR at column 0.
#define ONLRET	0000040 // The NL character is assumed to do the carriage-return function; the kernel's
                        // idea of the current column is set to 0 after both NL and CR.

/* c_cflag bit meaning */

/* c_lflag bits */
#define ISIG	0000001 // When any of the characters INTR, QUIT, SUSP, or DSUSP are received,
                        // generate the corresponding signal.
#define ICANON	0000002 // Enable canonical mode
#define XCASE	0000004
#define ECHO	0000010 // Echo input characters.
#define ECHOE	0000020 // If ICANON is also set, the ERASE character erases the preceding
                        // input character, and WERASE erases the preceding word.
#define ECHOK	0000040 // If ICANON is also set, the KILL character erases the current line.
#define ECHONL	0000100
#define NOFLSH	0000200 // Disable flushing the input and output queues when generating signals
                        // for the INT, QUIT, and SUSP characters.
#define TOSTOP	0000400 // Send the SIGTTOU signal to the process group of a background process
                        // which tries to write to its controlling terminal.
#define ECHOCTL	0001000 // If ECHO is also set, terminal special characters other than TAB, NL,
                        // START, and STOP are echoed as ^X, where X is the character with ASCII
                        // code 0x40 greater than the special character. For example, character
                        // 0x08 (BS) is echoed as ^H.
#define ECHOPRT	0002000
#define ECHOKE	0004000
#define FLUSHO	0010000
#define PENDIN	0040000
#define IEXTEN	0100000 // Enable implementation-defined input processing. This flag, as well as
                        // ICANON must be enabled for the special characters EOL2, LNEXT, REPRINT,
                        // WERASE to be interpreted, and for the IUCLC flag to be effective.

/* c_cc characters */
#define VINTR 0     // 003, ETX, Ctrl-C
#define VQUIT 1     /* 034, FS, Ctrl-\ */
#define VERASE 2    // 010, BS, Ctrl-H
#define VKILL 3     // 025, NAK, Ctrl-U, Ctrl-X. This erases the input since the last
                    // EOF or beginning-of-line. Recognized when ICANON is set, and
                    // then not passed as input.
#define VEOF 4      // 004, EOT, Ctrl-D. More precisely: this character causes the pending tty
                    // buffer to be sent to the waiting user program without waiting for end-of-line.
                    // If it is the first character of the line, the read(2) in the user program
                    // returns 0, which signifies end-of-file. Recognized when ICANON is set, and
                    // then not passed as input.
#define VTIME 5     // Timeout in deciseconds for noncanonical read (TIME)
#define VMIN 6      // Minimum number of characters for noncanonical read (MIN)
#define VSWTC 7
#define VSTART 8    // 021, DC1, Ctrl-Q
#define VSTOP 9     // 023, DC3, Ctrl-S. Stop output until Start character typed.
                    // Recognized when IXON is set, and then not passed as input.
#define VSUSP 10    // 032, SUB, Ctrl-Z. Suspend character (SUSP). Send SIGTSTP signal.
                    // Recognized when ISIG is set, and then not passed as input.
#define VEOL 11     // 000, NUL. Additional end-of-line character (EOL). Recognized when ICANON is set.
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16

#endif /* _TERMIOS_H */
