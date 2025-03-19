/*** includes ***/
// feature test macros may need to be defined here for getline(), for example
// // macros are added above our includes, because the header files we're including use the macros to decide which features to expose
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h> // gives us: iscntrl()
#include <errno.h> // gives us: EAGAIN and errno
#include <stdio.h> // gives us: FILE, fopen(), getline(), perror(), printf(), snprintf(), sscanf()
#include <stdlib.h> // gives us: atexit(), exit(), free(), malloc(), realloc()
#include <string.h> // gives us: memcpy(), strlen()
#include <sys/ioctl.h> // gives us icoctl(), TIOCGWINSZ, struct winsize
#include <sys/types.h> // gives us ssize_t
#include <termios.h>  // gives us: struct termios, tcgetattr(), tcsetattr(), ECHO, ICANON, ICRNL, IXTEN, ISIG, IXON, TCSAFLUSH, and also BRKINT, INPCK, ISTRIP, and CS8. also VMIN and VTIME
#include <unistd.h> // gives us: standard symbolic constants and types, also write() and STDOUT_FILENO

/*** defines ***/

#define KILO_VERSION "0.0.1"

// the CTRL_KEY macro bitwise-ANDS a character with the value 00011111, essentially setting the upper 3 bits to 0, mirroring what the Ctrl key does in the terminal
// // the ASCII character set seems designed this way on purpose. Similarly it is designed so that you can set and clear bit 5 to switch between lowercase and uppercase
// // in C bitmasks are generally specified with hexadecimal, because C doesn't have binary literals
#define CTRL_KEY(k) ((k) & 0x1f)

// Here we give a representation for the arrows that doesn't conflict with w,a,s,d or any other chars, so we'll use ints that are out of char's range
// // by setting the first constant in the enum to 1000, the rest get incrementing values of 1001, 1002, 1003, etc
enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/

// erow here stands for "editor row" and stores a line of text as a pointer to the dynamically-allocated character data and a length. the typedef lets us refer to the type as erow instead of struct erow
typedef struct erow {
    int size;
    char *chars;
} erow;

// a global struct that will contain our editor state
struct editorConfig {
    int cx, cy; // variables for holding cursor column and row location
    int rowoff; // row offset to keep track of which row of the file the user is currently scrolled to
    int screenrows; // variable for screen height
    int screencols; // variable for screen width
    int numrows;
    erow *row; 
    struct termios orig_termios; // here we store the original terminal attributes in a global variable
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
    // clear screen and reposition cursor on exit due to error, but before printing an error, so it's not erased immediately
    write(STDIN_FILENO, "\x1b[2J", 4);
    write(STDIN_FILENO, "\x1b[H", 3);

    // most C library functions that fail will set the global errno value to indicate the error
    // perror will look at errno and print a descriptive error message for its value
    perror(s);
    // exiting with an exit status of 1 (or any other non-zero value) indicates failure
    exit(1);
}

void disableRawMode(void) {
    // run tcsetattr() with those arguments and return an error with die() if it fails
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode(void) {
    // we use tcgetattr() here to read current attributes into a struct
    // call die() if it fails
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    // struct we create
    struct termios raw = E.orig_termios;

    // here we modify the struct
    // // ECHO normally causes each typed key to be printed to the terminal so you can see what you're typing, but we don't want that for what we're doing here
    // // // an example of a time where this is turned off is when you're entering a password in terminal, e.g. when using sudo
    // // c_lflag field is for "local flags", which we can think of as 'miscellanious flags'
    // // // other flag fields are c_iflag (input flags), c_oflag (output flags), and c_cflag (control flags)
    // // // ECHO is a bitflag which we can use the bitwise-NOT operator ~, then bitwise-AND & to force the 1 in ECHO to 0 while keeping every other bit's original value
    // by turning off BRKINT we prevent break conditions from sending a ctrl-c-like SIGINT from being sent
    // by turning off ICRNL we can have ctrl-m without carriage return new line processing as normal
    // by turning off INPCK we remove parity checking, which most modern terminal emulators don't seem to need
    // by turning off ISTRIP we prevent stripping off the 8th bit of each byte (that would set it to 0), though this is probably already turned off on most terminal emulators
    // by turning off IXON we can enter ctrl-s and ctrl-q without affecting data transmission to the terminal
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // by turning off OPOST we disable all output processing like "\r\n" (carriage return new line)
    raw.c_oflag &= ~(OPOST);
    // turn off CS8 setting character size to 8 bits per byte. probably already turned off, but just in case
    // // we set this with just | (OR) unlike the others
    raw.c_cflag |= (CS8);
    // by turning off ICANON we will read be reading input byte-by-bite instead of line-by-line
    // by turning off IEXTEN we can have ctrl-c functionality without the terminal thinking we want to wait to enter a character
    // by turning off ISIG we can have ctrl-c functionality without stopping or suspending the program
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // VMIN sets the minimum number of bytes of input before read() can return
    raw.c_cc[VMIN] = 0;
    // VTIME sets the minimum amount of time before read() can return, in tenths of a second
    raw.c_cc[VTIME] = 1;

    // here we pass the modified struct to tcsetattr() to write the new terminal attributes back out
    // // TCSAFLUSH argument specifies when to apply the change
    // // // in this case, it waits for all pending output to be written to the terminal, and discards an unread input
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// this function waits for one keypress and returns it
int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        // we make the seq buffer 3 bytes long to handle longer escape sequences in the future
        char seq[3];

        // each of these read()s will time out after 0.1 seconds, which is long enough for the near instantaneous escape sequences produced by arrow key presses to trigger them, but will otherwise assume the user just pressed the Escape key and return that
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

// this function is just fallback method in case the getWindowSize() method doesn't work on some system
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] ='\0';

    // first we make sure it responded with an escape sequence
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    // then we pass a pointer to the third character if buf to sccanf(), skipping '\x1b' and '[', so we are passing a string of the form 24;80 to sccanf(). We are also passing it the string %d;%d which tells it to parse two integers separated by a ;, and we put those values into the rows and cols variables
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    // on success, ioctl() will place the number of columns and rows of the terminal into the given winsize struct
    // another possible error is that the values returned are 0, so we check for that as well
    // // if the call to ioctl succeeded, we pass the values back by setting the int references that were passed into the function, this is a common approach to having functions return multiple values in C and allows you to use return value to indicate success or failure
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // if ioctl() doesn't work properly, as may be the case on some systems, we move the cursor to the bottom-right of the screen, then use escape sequences that let us query the the position of the cursor. this code moves the cursor to the bottom-right of the screen
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
        return -1;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

/*** file i/o ***/

// editorOpen() will eventually be for opening and reading a file from disk, so we put this in a new section
void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) die ("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // getline is useful for reading lines from a file when we don't know how much memory to allocate for each line. it takes care of memory management for us
    // first we pass it a NULL line pointer and a linecap (line capacity) of 0. that makes it allocate new memory for the next line it reads, and set line to point to the memory, and set linecap to let us know how much memory it allocated
    // // its return value is the length of the line it read, or -1 if it's at the end of file and there are no more lines to read
    linelen = getline(&line, &linecap, fp);
    if (linelen != -1) {
        // we also strip off the newline or carriage return at the end of the line before copying it into our erow. we know each erow represents one line of text, so we don't need to store a newline character at the end
        while ((linelen = getline(&line, &linecap, fp)) != -1) {
            while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
                linelen--;
            }
            editorAppendRow(line, linelen);
        }
    }
    free(line);
    fclose(fp);
}

/*** append buffer ***/

// rather than make many calls to write() for the many lines of the terminal window, we want to add a line buffer to the tildes and write them all at once when the window is refreshed

struct abuf {
    char *b;
    int len;
};

// this constant represents an empty buffer and acts as a constructor for our abuf type
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    // we have to allocate enough memory to hold the new string resulting from appending string s to a buf
    // we have realloc() give us a block of memory the size of the current string plus the size of the string we're appending
    // realloc() will either extend the size of the block of memory we have allocated, or it will free() the current block of memory and allocate a new block of memory somewhere else big enough for our new string
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    // we use memcpy() to copy the string s after the end of the current data in the buffer, and we update the pointer and length of the abuf to the new values
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

// this function is a destructor that deallocates the dynamic memory used by an abuf
void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

void editorScroll(void) {
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    int y;
    // this prints out tildes at the beginning of each row
    for (y = 0; y < E.screenrows; y++) {
        // this outer if-statement checks whether we are currently drawing a row that is part of of the text buffer, or a row that comes after the end of the text buffer
        // to draw a row that's part of the text buffer, we write out the chars field of the erow, but first we truncate the rendered line if it would go past the end of the screen
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
                // we truncate the length of the string in case the terminal is too small to fit the welcome message
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].size;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, E.row[filerow].chars, len);
        }

        // this is to clear each line as it is redrawn, rather than clearing the entire screen before each refresh
        // // the K command erease part of the current line, with a default argument of 0 erasing the part of the line to the right of the cursor, since we want that here, we leave out the 0 and just use <esc>[K
        abAppend(ab, "\x1b[K", 3);
        // this if statement checks to see if we're on the bottom row of the terminal window. if not, we do a new line carriage return
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        } 
    }
}

void editorRefreshScreen(void) {
    editorScroll();
    // here we initialize a new abuf, ab, by assigning ABUF_INIT to it. we replace each occurrence of write(STDOUT_FILENO, ...) with abAppend(&ab, ...). we also pass ab into editorDrawRows(), so it can use abAppend() too. lastly, we write() the buffer's contents out to standard output, then ffree the memory used by the abuf
    struct abuf ab = ABUF_INIT;
    // this will clear the screen after each keypress. we are writing an escape sequence. 27, followed by [, then J which clears the screen with the argument 2, which clears the entire screen. 1 would clear it up to where the cursor is. 0 would clear it from the cursor up to the end of the screen, and 0 is the default argument
    // // \x1b is the escape character (27 in decimal), [2J
    // // // we are using VT100 escape sequences, suported very widely in modern terminal emulators. if we wanted to support the maximum number of terminals, we could use the ncurses library, which uses the terminfo database to figure out a terminal's capabilities and which escape sequences to use for that particular terminal
    // lines under commented out code below may affect accuracy of comments above, somewhat
    // write(STDIN_FILENO, "\x1b[2J", 4);
    // write(STDIN_FILENO, "\x1b[H", 3);

    // this will hide the cursor before the screen refreshes, in order to avoid it appearing anywhere odd while the screen refreshes
    abAppend(&ab, "\x1b[?25l", 6);
    // this would have cleared the entire screen before each refresh
    // abAppend(&ab, "\x1b[2J", 4);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[32];
    // here the H command specifies the exact position we want the cursor to move to
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    // this should un-hide the cursor after the screen is drawn
    abAppend(&ab, "\x1b[?25h", 6);

    // reposition the cursor at the top-left after drawing tildes
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

// Here we make it so that pressing a or d decrements or decrements E.cx to move the cursor left or right
// pressing w or s decrements or decrements E.cy to move the cursor up or down

void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            // TODO: simplify these 4 if-statements to be one line each
            if (E.cx != 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screencols - 1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }
}

// this function waits for a keypress, then handles it
void editorProcessKeypress(void) {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            // clear screen and reposition cursor on intentional exit
            write(STDIN_FILENO, "\x1b[2J", 4);
            write(STDIN_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = E.screencols -1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                // variables can't be created directly in a switch statement, but we can create a code block with these braces that allows us to create a variable
                int times = E.screenrows;
                while (times--) {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.numrows = 0; // at first the editor will only display a single line of text, and so numrows can be either 0 or 1, we'll initialize it to 0 here
    E.row = NULL; // we'll make this a dynamically-allocated array of erow structs, initalized to NULL

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    // initEditor will initialize all the fields in the E struct
    initEditor();
    // we allow the user to choose a file to open by checking if they passed a filename as a command line argument
    // if they did not, we run with no arguments and editorOpen() will not be called, so they'll start with a blank file
    if (argc >= 2) {
        // if they did pass a filename as a command line argument, we call editorOpen() and pass it that filename
        editorOpen(argv[1]);
    }

    // commenting out a lot of code which will still be useful to look at later
    // char c;
    // // while loop will read 1 byte from standard input into char c until there are no more bytes left to read
    // // // read returns the number of bytes that it read, and returns 0 when it reaches the end of a file
    // // // // terminal by default starts in canonical (cooked) mode where keyboard input is only sent to your program when user hits Enter. We want to change to raw mode by turning off certain flags in the terminal
    // while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
    //     // we now have to manually add in \r for carriage returns because we disabled that above
    //     if (iscntrl(c)) {
    //         // if a character is a control characcter, it won't be printable to the screen, but its ASCII value will be printed here
    //         printf("%d\r\n", c);
    //     } else {
    //         // prints out a non-control character and its ASCII value
    //         printf("%d ('%c')'\r\n", c, c);
    //     }
    // }

    // // the following code modifies the preceding code
    // while (1) {
    //     char c = '\0';
    //     // EAGAIN is the errno value given by Cygwin when read() times out, so we won't treat it as an error
    //     if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");
    //     if (iscntrl(c)) {
    //         printf("%d\r\n", c);
    //     } else {
    //         printf("%d ('%c')'\r\n", c, c);
    //     }
    //     // following 3 lines can be rewritten as:
    //     // if (c == 'q') break;
    //     if (c == CTRL_KEY('q')) {
    //         break;
    //     }
    // }

    // the following code replaces the previous code with new functionality
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}