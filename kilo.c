/*** includes ***/
// feature test macros may need to be defined here for getline(), for example
// // macros are added above our includes, because the header files we're including use the macros to decide which features to expose
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h> // gives us: iscntrl()
#include <errno.h> // gives us: EAGAIN and errno
#include <fcntl.h> // gives us: open(), O_CREAT, O_RDWR
#include <stdarg.h> // gives us va_end(), va_start(), va_list
#include <stdio.h> // gives us: FILE, fopen(), getline(), perror(), printf(), snprintf(), sscanf(), vsnprintf()
#include <stdlib.h> // gives us: atexit(), exit(), free(), malloc(), realloc()
#include <string.h> // gives us: memcpy(), memmove(), memset(), strchr(), strcmp(), strdup(), strerror(), strlen(), strncmp(), strrchr(), strstr()
#include <sys/ioctl.h> // gives us: icoctl(), TIOCGWINSZ, struct winsize
#include <sys/types.h> // gives us: ssize_t
#include <termios.h>  // gives us: struct termios, tcgetattr(), tcsetattr(), ECHO, ICANON, ICRNL, IXTEN, ISIG, IXON, TCSAFLUSH, and also BRKINT, INPCK, ISTRIP, and CS8. also VMIN and VTIME
#include <time.h> // gives us: time(), time_t
#include <unistd.h> // gives us: standard symbolic constants and types, also close(), ftruncate(), write() and STDOUT_FILENO

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 2

// the CTRL_KEY macro bitwise-ANDS a character with the value 00011111, essentially setting the upper 3 bits to 0, mirroring what the Ctrl key does in the terminal
// // the ASCII character set seems designed this way on purpose. Similarly it is designed so that you can set and clear bit 5 to switch between lowercase and uppercase
// // in C bitmasks are generally specified with hexadecimal, because C doesn't have binary literals
#define CTRL_KEY(k) ((k) & 0x1f)

// Here we give a representation for the arrows that doesn't conflict with w,a,s,d or any other chars, so we'll use ints that are out of char's range
// // by setting the first constant in the enum to 1000, the rest get incrementing values of 1001, 1002, 1003, etc
enum editorKey {
    BACKSPACE = 127,
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

// here we'll have each value in the hl array corespond to a character in render, and it will tell us whether that character is part of a string, comment, number, etc. and this enum will contain the possible values hl can contain
enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/

struct editorSyntax {
    char *filetype; // this will hold the name of the filetype displayed to the user in the status bar
    char **filematch; // this will be an array of strings where each string contains a pattern to match a filename against. if the filename matches, then the file will be recognized as having that filetype
    char **keywords; // this will be a NULL-terminated array of strings, each string containing a keyword
    char *singleline_comment_start; // we'll let each language specify its own pattern for single-line comments, because they can differ so much between languages
    int flags; // this will be a bit field that will contain flags for whether to highlight numbers and whether to highlight strings for that filetype
};

// erow here stands for "editor row" and stores a line of text as a pointer to the dynamically-allocated character data and a length. the typedef lets us refer to the type as erow instead of struct erow
typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl; //hl here stands for highlight. this is an array of unsigned char values, meaning integers in the range 0 to 255
} erow;

// a global struct that will contain our editor state
struct editorConfig {
    int cx, cy; // variables for holding cursor column and row location
    int rx;
    int rowoff; // row offset to keep track of which row of the file the user is currently scrolled to
    int coloff; // like rowoff but for columns
    int screenrows; // variable for screen height
    int screencols; // variable for screen width
    int numrows;
    erow *row;
    int dirty; // tells us whether the file has been changed since opening
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct termios orig_termios; // here we store the original terminal attributes in a global variable
};

struct editorConfig E;

/*** filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else", "struct", "union", "typedef", "static", "enum","class", "case",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|", "void|", NULL
};

// HLDB here is our "highlight database"
struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
        // the above bit flags will be turned on when highlighting C files
    },
};

// this is a constant that will store the length of the HLDB array
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

// we have to create a function prototype here because we are calling editorSetStatusMessage() in a function above where it is called, which is not supposed to be possible in a language like C that is designed to compile in a single pass through the program
// // when we call a function in C, the compiler needs to know the arguments and return value of that function, we can tell the compiler this information here near the top of the file
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

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
        // return -1; // i think i left this here by mistake
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

int is_separator(int c) {
    // strchr() looks for the first occurrence of a character in a string, then returns a pointer to the matching character in the string. if the string doesn't contain the character, it returns NULL
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[]", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
    // first we realloc() the needed memory, since the row might be bigger than the last time we highlighted it, or it could be a new row
    // // note that the size of the hl array is the same as the size of the render array, so we use rsize as the amount of memory to allocate
    row->hl = realloc(row->hl, row->rsize);
    // we use memset to set all characters to HL_NORMAL by default, before looping through the characters and setting the digits to HL_NUMBER
    memset(row->hl, HL_NORMAL, row->rsize);

    // if no filetype is set, we return immediately after memset()ing the entire line to HL_NORMAL
    if (E.syntax == NULL) return;

    // we make keywords an alias for E.syntax->keywords for similar reasons to doing so for scs
    char **keywords = E.syntax->keywords;

    // we make scs an alias for E.syntax->singleline_comment_start for easier typing and better readability
    char *scs = E.syntax->singleline_comment_start;
    // we set scs_len to the length of the string, or 0 if the string is NULL. this lets us use scs_len as a boolean to know whether we should highlight single-line comments
    int scs_len = scs ? strlen(scs) : 0;

    // we initialize prev_sep to 1 (meaning true) because we consider the beginning of hte line to be a separator. without this, numbers at the very beginning of the line wouldn't be highlighted
    int prev_sep = 1;
    // in_string will track whether we're currently inside of a string and allow us to keep highlighting the current character as a string until we hit the closing quote
    int in_string = 0;

    // changing this for-loop to a while-loop allows us to consume multiple characters in each iteration, though we'll still only consume one character a time when processing numbers
    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        // prev_hl is set to the highlight type of the previous character
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        // this if-statement checks scs_len and also makes sure we're not in a string, since we're placing this code above the string highlighting code, and order matters a lot for highlighting purposes
        if (scs_len && !in_string) {
            // this if statement checks if the character is the start of a single-line comment
            if (!strncmp(&row->render[i], scs, scs_len)) {
                // if the character is the start of a single-line comment, then we memset() the rest of the line with HL_COMMENT and break out of the syntax highlighting loop, and the line is done being highlighted
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            // if in_string is set then we know the current character can be highlighted with HL_STRING
            if (in_string) {
                row->hl[i] = HL_STRING;
                // here we account for escape quotes, because in most programming languages, \' and \" don't close strings
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                // we set in_string to a double or single quote so that we know which one closes the string, so if we come across the closing quote, we set to in_string to 0
                if (c == in_string) in_string = 0;
                // we increment i to "consume" the current character
                i++;
                // we consider the closing quote to be a separator so we set prev_sep to 1
                prev_sep = 1;
                // then we continue out of the current loop iteration
                continue;
            } else {
                // if we're not currently in a string, then we have to check if we're at the beginning of one by checking for a double or single quote
                if (c == '"' || c == '\'') {
                    // if we find that we are in a string, we store the quote in in_string
                    in_string = c;
                    // then we highlight it with HL_STRING
                    row->hl[i] = HL_STRING;
                    // then we "consume" it by iterating i
                    i++;
                    continue;
                }
            }
        }

        // to highlight a digit with HL_NUMBER we now require the previous character to be a separator, or to also be highlighted with HL_NUMBER
        // we're also accounting for decimals here by highlighting a '.' that comes after a character we just highlighted as a number
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            // the above if-statement checks to see if numbers should be highlighted for the current filetype
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                // when we decide to highlight the current character a certain way, we increment i to "consume" that character, set prev_sep to 0 to indicate that we're in the middle of highlighting something, and then continue the loop
                i++;
                prev_sep = 0;
                continue;
            }
        }

        // keywords require a separator before and after the word to avoid highlighting the middle of certain words etc, so we check that a separator comes before the word here
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]); // here we store the length of the keyword
                int kw2 = keywords[j][klen - 1] == '|'; // here we store whether it's a secondary keyword
                if (kw2) klen--; // if it's a secondary keyword, we decrement klen to account for the extraneous | pipe character

                // we use strncmp() to check if the keyword exists at our current position in the text, and we also check to see if a separator comes after the keyword. because \0 is considered a separator, this also works if the keyword is at the very end of the line
                if (!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i + klen])) {
                    // if all those conditions are satisfied, then we use memset() to highlight the whole keyword at once, with color depending on the value of kw2
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    // then we consume the entire keyword, incrementing i by its length
                    i += klen;
                    break; // we break instead of continuing because we are in an inner loop
                }
            }

            // we check if the loop was broken out of by seeing if it got to the terminating NULL value, and if so then we continue
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        // if we end up not highlighting the previous character, then we end up here where we set prev_sep according to whether the current character is a separator and increment i to consume the character. the memset() we did at the top of the function means that an unhighlighted character will have a value of HL_NORMAL in hl
        prev_sep = is_separator(c);
        i++;
    }
}

int editorSyntaxToColor(int hl) {
    switch (hl) {
        // we're handling HL_NORMAL separately so we don't include it here
        case HL_COMMENT: return 36; // comments will be rendered in cyan
        case HL_KEYWORD1: return 33; // keywords defined here will render yellow
        case HL_KEYWORD2: return 31; // keywords defined here will render red
        case HL_STRING: return 35; // any string enclosed by quotes will be rendered magenta
        case HL_NUMBER: return 32; // any number will be rendered green
        case HL_MATCH: return 34; // any match to a search query will be rendered blue
        default: return 37; // anything else will be rendered white
    }
}

// this function will try to match the current filename to one of the filematch fields in HLDB. if one matches, it'll set E.syntax to that filetype
void editorSelectSyntaxHighlight(void) {
    // we set E.syntax to NULL first so that if there is no filename, then there is no filetype
    E.syntax = NULL;
    if (E.filename == NULL) return;

    // strrchr() returns a pointer to the last occurrence of a character in a string
    // if there is no extension, then ext will be NULL
    char *ext = strrchr(E.filename, '.');

    // we loop through each editorSyntax struct in the HLDB array, and for each of those we loop through each pattern in its filematch array
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            // strcmp() returns 0 if two given strings are equal
            // if the pattern stats with a . then it's a file extension pattern and we use strmp() to see if the filename ends with that extension. if it's not a file extension pattern then we just check to see if the pattern exists anywhere in the filename, using strstr(). 
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[i]))) {
                // if the filename matched according to those rules, then we set E.syntax to the current editorSyntax struct, and return
                E.syntax = s;
                
                int filerow;
                // in order to update the highlighting for the entire file after setting E.syntax, we loop through each row in the file and call editorUpdateSyntax() on it. this ensures that highlighting immediately changes when the filetype changes
                for (filerow = 0; filerow < E.numrows; filerow++) {
                    editorUpdateSyntax(&E.row[filerow]);
                }

                return;
            }
            i++;
        }
    }
}

/*** row operations ***/

// this function converts a chars index into a render index.
int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;

    // for each character, if it's a tab we use rx % KILO_TAB_STOP to find out how many columns we are to the right of the last tab stop, then subtract that from KILO_TAB_STOP - 1 ti find out how many columns we are to the left of the next tab stop. we add that amount to rx to get just to the left of the next tab stop, and then the unconditional rx++ statement gets us right on the next tab stop. This works even if we are currently on a tab stop.
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    // to convert an rx to a cx we reverse the function for doing the opposite. we loop through the chars string, calculating current rx value (cur_rx) as we go. we want to stop when cur_rx hits the given rx value and return cx
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        }
        cur_rx++;

        // this return statement should handle all rx values that are valid indexes into render
        if (cur_rx > rx) return cx;
    }
    // this return statement is just in case the caller provides an rx that's out of range, which shouldn't happen
    return cx;
}

// this function uses the chars string of an erow to fill in the contents of the render string. we'll copy each character from chars to render
void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    // first we have to loop through the chars of the row and count the tabs in order to know how much memory to allocate for render
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

    
    int idx = 0;
    // this for loop idx contains the number of characters we copied into row->render so we assign it to row->rsize
    for (j = 0; j < row->size; j++) {
        // the maximum number of characters needed for each tab is 8. row->size already counts 1 for each tab, so we multiply the number of tabs by 7 and add that to row->size to get the maximum amount of memory we'll need for that rendered row
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    // we call editorUpdateSyntax() here because editorUpdateRow() already has the job of updating the render array whenever the text of a row changes, so it makes sense that this is where we'd want to update the hl array, after we've updated render above
    editorUpdateSyntax(row);
}

// erow gets constructed and initialized here
void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    // int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++; // any change to the file will set the dirty flag to not equal 0
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

// this is similar to editorRowDelChar() because here and there we are deleting a single element from an array of elements by its index
void editorDelRow(int at) {
    // first we validate the at index
    if (at < 0 || at >= E.numrows) return;
    // then we free the memory owned by the row using editorFreeRow()
    editorFreeRow(&E.row[at]);
    // then we use memmove() to overwrite the deleted row struct with the rest of the rows that come after it
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    // then we decrement rows and increment the dirty flag
    E.numrows--;
    E.dirty++;
}

// this function inserts a single character into an erow, at a given position
void editorRowInsertChar(erow *row, int at, int c) {
    // first we validate at, which is the eindex where we want to insert the character. at is allowed to go one character past the end of the string, in which case the character should be inserted at the end of the string
    if (at < 0 || at > row->size) at = row->size;
    // then we allocate one more byte for the chars of the erow (we add 2 because we also need room for the null byte)
    row->chars = realloc(row->chars, row->size + 2);
    // memmove is like memcpy() but safe to use when the  source and destination arrays overlap
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    // we increment the size of the characters array
    row->size++;
    // then we assign the character to its position in the array
    row->chars[at] = c;
    // we call editorUpdateRow() so that the render and rsize fields get update with the new row content
    editorUpdateRow(row);
    E.dirty++; // any change to the file will set the dirty flag to not equal 0
}

// similar to editorRowInsertChar() but there's no memory management to do
void editorRowDeleteChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    // we use memmove() to overwrite the deleted character with the characters that come after it (including the null byte)
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    // then we decrement the row's size, update the row, and set the dirty flag
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    // the row's new size is row->size + len + 1 so we allocate that much memory for row->chars
    memcpy(&row->chars[row->size], s, len);
    // then we update row->size
    row->size += len;
    // then we add a null byte to the end of the row
    row->chars[row->size] = '\0';
    // then we update the row and make sure to increment E.dirty indicate the document has been changed
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

// this section contains functions that we call from editorProcessKeys() when we're mapping keypresses to various text editiing operations

// this function will take a character and use editorRowInsertChar() to insert that character into the position that the cursor is at
// // note that this function doesn't have to handle the details of modifying an erow, and editorRowInsertChar() doesn't have to handle the cursor's location. this is the reason we have some functions in /editor operations/ and other functions in /row operations/
void editorInsertChar(int c) {
    // this if statement checks if the cursor is on the tilde line after the end of the file. if it is, then the cursor is on the tilde line after the end of the file, so we need to append a new row to the file before inserting a character there
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    // after inserting the character we move the cursor forward so that the next character the user inserts will go after the one they've just inserted
    E.cx++;
}

void editorInsertNewLine(void) {
    // if we're at the beginning of a line, we only have to insert a new blank row before the line we're on
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    // if we're not at the beginning of a line, we have to split the current line into row rows
    } else {
        erow *row = &E.row[E.cy];
        // first we call editorInsertRow() and pass it the characters on the current row after the cursor. this creates a new row after the current one with all the characters that were previously in the current row
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx); // this called function includes a call to editorUpdateRow() for the new row
        // then we reassign the row pointer because editorInsertRow() calls realloc(), which may move memory around and invalidate the pointer
        row = &E.row[E.cy];
        // then we truncate the current row's contents by setting its size to the position of the cursor
        row->size = E.cx;
        row->chars[row->size] = '\0';
        // then we call editorUpdateRow() on the truncated row
        editorUpdateRow(row);
    }
    // in both cases we increment E.cy since there's a new row in the file
    E.cy++;
    // we also set E.cx to 0 to move the cursor to the beginning of the row
    E.cx = 0;
}

void editorDelChar(void) {
    // if the cursor is past the end of the file so there's nothing to delete and we do nothing
    if (E.cy == E.numrows) return;
    // we're at the beginning of the first line so there's nothing to do and we do nothing
    if (E.cx == 0 && E.cy == 0) return;

    // if the cursor is within the bounds of the file we get the erow the cursor is on
    erow *row = &E.row[E.cy];
    // then we check if there's a character to the left of the cursor, delete it, and move the cursor one space to the left
    if (E.cx > 0) {
        editorRowDeleteChar(row, E.cx - 1);
        E.cx--;
    } else {
        // if we find that E.cx == 0, we call editorAppendString() and editorDelRow as we planned
        // row points to the row we are deleting, so we append row->chars to the previous row, then delete the row that E.cy is on
        // then we set E.cx to the end of the contents of the previous row before appending that row, that way the cursor will end up at the point where the two lines joined
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

// this function converts our array of erow structs into a single string ready to be written out to a file
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int j;
    // first we add the lengths of each row of text, adding 1 to the total length each time for the newline character we'll add to the end of each line
    for (j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    // we save the total length into buflen to tell the caller how long the string is
    *buflen = totlen;

    // we allocate the required memory
    char *buf = malloc(totlen);
    char *p = buf;
    // then we loop through the rows and memcpy() the contents of each row to the end of the buffer, appending a newline character after each row
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    // lastly, we return buf, and we expect the caller to free() the memory
    return buf;
}

// editorOpen() will eventually be for opening and reading a file from disk, so we put this in a new section
void editorOpen(char *filename) {
    free(E.filename);
    // strdup() makes a copy of the given string, allocating the required memory and assuming you will free() that memory
    E.filename = strdup(filename);

    // we set the syntax highlighting here based on what kind of file was opened
    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if (!fp) die ("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // getline is useful for reading lines from a file when we don't know how much memory to allocate for each line. it takes care of memory management for us
    // first we pass it a NULL line pointer and a linecap (line capacity) of 0. that makes it allocate new memory for the next line it reads, and set line to point to the memory, and set linecap to let us know how much memory it allocated
    // // its return value is the length of the line it read, or -1 if it's at the end of file and there are no more lines to read
    // we also strip off the newline or carriage return at the end of the line before copying it into our erow. we know each erow represents one line of text, so we don't need to store a newline character at the end
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }
        editorInsertRow(E.numrows, line, linelen);
        }
    free(line);
    fclose(fp);
    E.dirty = 0; // editorOpen() calls editorAppendRow() which increments E.dirty, even without the user making any changes, so we want to reset E.dirty after opening the file ---- editorAppendRow() has been changed to editorInsertRow
}

// this function will actually write the string return by editorRowsToString() to the disk
void editorSave(void) {
    // if it's a new file, then E.filename will be NULL
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        // here we handle the user pressing Escape, which would result in NULL being returned
        if (E.filename == NULL) {
            editorSetStatusMessage("Save cancelled");
            return;
        }
        // we set the syntax highlighting again here in case the file didn't have a name before, or it was changed in the editing process
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    // we tell open to create a new file if one doesn't already exist, where we have to pass an extra argument containing the mode (permissions) for the new file, so here we use 0644 as iits the standard set of permissions for a text file that the owner wants to read and write to while only letting others read it
    int fd = open(E.filename, O_RDWR  | O_CREAT, 0644);
    // open() and ftruncate() both return -1 on error
    if (fd != -1) {
        // ftruncate() sets the file's size to a specified length. if its larger than that, it will cut off any data at the end of the file to make it fit that length. if the file is shorter, it will add 0 bytes to the end to make it that length
        // the normal way to overwrite a file is to pass the O_TRUNC flag to open() which truncates the file completely, by making it an empty file before writing the new data into it.
        // // by truncating the file ourselves to the same length as the data we're planing to write into it, we're making the whole overwriting operation a bit safer in case ftruncate() succceeds but write() fails. in that case the file would still contain most of the data it had before, as opposed to the case of truncating the file completely and then having write() fail, which would result in all of the data being lost
        // // // more advanced editors will write to a new temporary file, and then rename that file to the actual file the user wants to overwrite, carefully checking for errors through the whole process.
        if (ftruncate(fd, len) != -1) {
            // we expect write() to return the number of bytes we told it to write
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0; // after saving the file, it will no longer have any unsaved changes, so we reflect that by resetting E.dirty here
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        // whether or not there was an errorm we still want to make sure we close the file and free the memory that buf points to
        close(fd);
    }
    free(buf);
    // because of the return statement above, we only reach this point if there was an error in the process of saving the file
    // // strerror() is like perror() but takes errno as an argument and returns the human-readable string for that error code so that we can make the error part of the status message displayed to the user
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

// in this callback, we check if the user pressed Enter or Escape, in which case they are leaving search mode so we return immediately instead of doing another search.
void editorFindCallback(char *query, int key) {
    // last match will contain the index of the row that the last match was on, or -1 if there was no last match
    static int last_match = -1;
    // direction will store the direction of the search, 1 for forwards and -1 for backwards
    static int direction = 1;

    static int saved_hl_line;
    // here we restore hl to the contents of saved_hl
    static char *saved_hl = NULL; // save_hl is a dynamically allocated array which points to NULL when there is nothing to restore

    if (saved_hl) {
        // if there is anything to restore, we memcpy() it to the saved line's hl and then deallocate saved_hl and set it back to NULL
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }
    
    // we always reset last_match to -1 unless an arrow key was pressed, so we only advanced to the next or previous match when that happens. direction is also always set to 1 unless the left or up keys are pressed. so we always search forward unless the user specifies otherwise
    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    // current is the index of the current row we're searching. if there was a last match, it starts on the line before (if searching forwards) or after (searching backwards). if there wasn't a last match, it starts at the top of the file and searches forward to find the first match
    int current = last_match;
    // if the user hasn't pressed Enter or Escape, any key press starts another search for the current query string
    int i;
    // looping through all the rows of the file here
    for (i = 0; i < E.numrows; i++) {
        current += direction;
        // this if-else-if statement causes current to go from the end of the file back to beginning, or from the begining to the end, to allow a search to "wrap around" the end of the file and continue from the top (or bottom)
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;

        erow *row = &E.row[current];
        // we use strstr() to check if query is a substring of the current row. it returns a pointer to the matching substring if there is a match, and otherwise returns NULL
        char *match = strstr(row->render, query);
        if (match) {
            // when we find a match we set last_match to current, so that if the user presses the arrow keys, the next search starts from that point
            last_match = current;
            E.cy = current;
            // if there is a match, we take the pointer to the matching substring and convert it into an index that we can set E.cx to, by substracting the row->render pointer from the match pointer, since match is a pointer into the row->render string
            // E.cx = match - row->render; // there is an issue here: E.cx is an index into chars, but we assigned it to a render index. if there are tabs to the left of the match, the cursor will be in the wrong position. we must convert the render index into a chars index before assigning it to E.cx
            E.cx = editorRowRxToCx(row, match - row->render); // this will now convert the matched index to a chars index and assign it to E.cx;

            // the last thing we do is set E.rowoff so that we're at the very bottom of the file, which will cause editorScroll() to scroll up at the next screen refresh so that the matching line will be at the very top of the screen. This will make it so the user doesn't have to look all over the screen to find where their cursor jumped or where the matching line is
            E.rowoff = E.numrows;

            // save_hl_line is another static variable we use to know which line's hl needs to be restored
            saved_hl_line = current;
            // here's we'll save the original contents of hl in a static variable named saved_hl
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            // we memset() the matched substring to HL_MATCH in our search code here. match - row->render is the index of the render of the match, so we use that as our index into hl
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

// when the user types a search query and presses Enter, we'll loop through all the rows of the file, and if a row contains their query string, we'll move the cursor to the match
// // most of the code formerly here was moved to editorFindCallback
void editorFind(void) {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (ESC/Arrows/Enter)", editorFindCallback);
    // if the user pressed Escape to cancel the input prompt, then editorPrompt() returns NULL, we cancel the search, and restore the cursor to where it was
    if (query){
        free(query);
    } else {
        // this will reset the cursor to its pre-search location
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
    
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
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
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
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            // we can longer use our previous approach of feeding the substring render that we want to print right into abAppend(), now we must do it character-by-character to handle color highlighting
            char *c = &E.row[filerow].render[E.coloff];
            // first we get a pointer, hl, to the slice of the hl array that corresponds to the slice of render that we are printing
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1; // we set this to -1 when we want the default text color, otherwise it's set to the value editorSyntaxToColor() last returned
            int j;
            // no longer up to date comment // we loop through the characters here and use isdigit() to check if a character is a number, if so then we precede it with the <esc>[32m escape sequence to make it green, then follow that with the escape sequence for the default color, <esc>[39m. the m here is the same argument we used to invert colors in the status bar
            // for each character we check if it's HL_NORMAL (where we'd use the default color) or something else. if it's something else then we use snprintf() to write the escape sequence into a buffer which we pass to abAppend() before appending the actual character
            for (j = 0; j < len; j++) {
                // first we'll handle the conversion of non-printable characters to printable ones to handle edge cases, like a user opening up a file that wouldn't normally be expected in a text editor, like the executable for this program for example
                // first we check if the current character is a control character
                if (iscntrl(c[j])) {
                    // if the current character is a control character, we translate it into a printable character by adding its value to '@' or '?' depending on if it's in the alphabetic range or not
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    // we use <esc>[7m to switch to inverted colors before printing the translated symbol
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    // we use <esc>[m to turn off inverted colors again, but this also turns off all text formatting, so we need to print the escape sequence for the corrent current color afterwards
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        // when we go from highlighted text back to HL_NORMAL we print out the normal color escape sequence and set current_color to -1
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                // if the color changes, we print out the escape sequence for that new color and set current_color to it
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            // after we're done loopthrough and display all the characters, we print a final <esc>[39m escape sequence to make sure the text color is reset to default
            abAppend(ab, "\x1b[39m", 5);
        }

        // this is to clear each line as it is redrawn, rather than clearing the entire screen before each refresh
        // // the K command erease part of the current line, with a default argument of 0 erasing the part of the line to the right of the cursor, since we want that here, we leave out the 0 and just use <esc>[K
        abAppend(ab, "\x1b[K", 3);
        // this if statement checks to see if we're on the bottom row of the terminal window. if not, we do a new line carriage return
        // if (y < E.screenrows - 1) {
        abAppend(ab, "\r\n", 2);
        // we removed this if-statement so that editorDrawRows() doesn't try to draw a line of text at the bottom of the screen, because now we want that line to be reserved for the status bar
        // } 
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    // the current line is stored in E.cy and we add 1 to that since E.cy is 0-indexed
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    // after printing the first status string we print spaces until we get to the point where if we printed the second status string, it would end up right against the edge of the screen. That would be when E.screencols - len is equal to the length of the second status string
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
            // if we reach the point described above, we break out of the loop, as the entire status bar is now being printed
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    // here we clear the message bar with the <esc>[K escape sequence
    abAppend(ab, "\x1b[K", 3);
    // then we make sure the message will fit the width of the screen
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    // then we display the message but only if it is less than 5 seconds old
    // // also the screen is only refreshed when we press a key, so a message will persist longer if no key is pressed
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
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
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    // here the H command specifies the exact position we want the cursor to move to
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    // this should un-hide the cursor after the screen is drawn
    abAppend(&ab, "\x1b[?25h", 6);

    // reposition the cursor at the top-left after drawing tildes
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// this function takes a format string and a variable number of arguments, similar to printf()
// // the ... argument makes this a variadic function. C deals with these by having you call va_start() and va_end() on a value of the type va_list. the last argument before the ... (fmt in this case) must be passed to va_start() so that the address of the next argument is known. then between the va_start() and va_end() calls, we would call va_arg() and pass it the type of the next argument (usually gotten from the given format string) and it returns the value of that argument. in this case we pass fmt and ap to vsnprintf() and it takes care of reading the format string and calling va_arg() to get each argument
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // we use vsnprintf to store the resulting string in E.statusmsg and set E.statusmsgtime to the current time with time()
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL); // passing NULL to time() retuns the current time as the number of seconds since 1 Jan 1970
}

/*** input ***/

// this function displays a prompt in the status bar for the user to enter a filename for a new file, and lets the user input a line of text after the prompt
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    // we initialize buf to the empty string
    buf[0] = '\0';

    // this infinite loop repeatedly sets the status message, refreshes the screen, and waits for a keypress to handle
    while (1) {
        // the prompt is expected to be a formatted string contain an %s, which is where the user's input will display
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        // this will allow the user to delete input when typing a response to the save prompt
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        // we'll use the Escape key to cancel the input prompt
        } else if (c == '\x1b') {
            editorSetStatusMessage(""); // we clear the prompt
            if (callback) callback(buf, c); // this if-statement allows the caller to pass NULL for the callback, in case they don't want to use a callback
            free(buf); // we free() the buf from memory ourselves
            return NULL;
        } else if (c == '\r') {
            // if the user's input is not empty, pressing Enter will clear the status message and return their input
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
            // if they input a printable character without hitting Enter, we append it to buf
            // we do also have to make sure that the input key isn't one of the special keys in the editorKey enum, so we test that by checking that the input key is in the range of a char, which is less than 128
        } else if (!iscntrl(c) && c < 128) {
            // if buflen reaches the maximum capacity we allocated (stored in bufsize) we double bufsize and sallocate that amount of memory before appending to buf
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            // we also make sure that buf ends with a \0 character because both editorSetStatusMessage() and the caller of editorPrompt() will use it to know where the string ends
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

// Here we make it so that pressing a or d decrements or decrements E.cx to move the cursor left or right
// pressing w or s decrements or decrements E.cy to move the cursor up or down

void editorMoveCursor(int key) {
    // E.cy is allowed to be one line past the last line of the file, so we use the ternary operator in case ARROW_RIGHT to check if the cursor is on an actual line. If it is, then the row variable will point to the erow that the cursor is on, and we'll check if E.cx is to the left of the end of that line before we allow the cursor to move to the right
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            // TODO: simplify these 4 if-statements to be one line each
            if (E.cx != 0) {
                E.cx--;
            // if cursor is at the left of the screen and not on the first line, pressing left will go to the end of the line above
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            // if cursor is at the right of the screen and not on the last line, pressing right will go to the start of the line below
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
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

    // Despite the change we made above to stop the cursor from moving beyond the end of its own line, it can still move up or down from the end of a longer line to beyond the end of a shorter line, so we fix that here.
    // first we have to set row again, since E.cy could point to a different line than before. Here we consider a NULL line to be of length 0
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    // if E.cx is to the right of the end of the line, we set it to the end of the line
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

// this function waits for a keypress, then handles it
void editorProcessKeypress(void) {
    static int quit_times = KILO_QUIT_TIMES;
    
    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewLine();
            break;

        case CTRL_KEY('q'):
            // clear screen and reposition cursor on intentional exit
            // this if-statement keeps track of how many times the user must press to quit, only allowing exit when it equals 0
            if (E.dirty && quit_times > 0) {
                char *s = quit_times == 1 ? "" : "s";
                // small change of mine to make sure "s" is not included in "times" if quit_times == 1
                editorSetStatusMessage("Warning! File has unsaved changes. Press Crt-Q %d more time%s to quit.", quit_times, s);
                quit_times--;
                return;
            }
            write(STDIN_FILENO, "\x1b[2J", 4);
            write(STDIN_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        // backspace has no human-readable backslash-escape representation in C so we make it part of the editorKey enum and assign it its ASCII value of 127
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            // pressing the delete key is equivalent to pressing Arrow Rigbt and Backspace in many systems and contexts, so we do that here
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

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

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        // this default: case in the switch statement makes it so that any keypress not mapped to another editor function will be inserted directly into the text being edited
        default:
            editorInsertChar(c);
            break;
    }
    // reset quit_times confirmation count after any other key press, for the next time the user tries to quit
    quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor(void) {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0; // at first the editor will only display a single line of text, and so numrows can be either 0 or 1, we'll initialize it to 0 here
    E.row = NULL; // we'll make this a dynamically-allocated array of erow structs, initalized to NULL
    E.dirty = 0; // setting this to 0 because by default, the file will be considred "unchanged" until we make changes. it will just be used as a boolean value but we will also increment it with each change instead of just setting it to 1, so that we can have a sense of how many changes have been made
    E.filename = NULL; // this will stay NULL if we run the program without arguments (meaning a file isn't opened)
    E.statusmsg[0] = '\0'; // initialized to an empty string so no message will be displayed by default
    E.statusmsg_time = 0; // will contain a timestamp when we set a status message
    E.syntax = NULL; // when set to null, it means there is no filetype for the current file, and no syntax highlighting should be done

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    // we decrement E.screenrows so that editorDrawRows() doesn't try to draw a line of text at the bottom of the screen
    E.screenrows -= 2; // we are doing 2 now for the message area under the status bar
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

    // initial status message shows key bindings our text editor currenly uses to quit
    editorSetStatusMessage("HELP: Ctrl-s = save | Ctrl-f = find | Ctrl-q = quit");

    // the following code replaces the previous code with new functionality
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}