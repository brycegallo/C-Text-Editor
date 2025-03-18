#include <ctype.h> // gives us: iscntrl()
#include <stdio.h> // gives us: printf()
#include <stdlib.h> // gives us: atexit()
#include <termios.h>  // gives us: struct termios, tcgetattr(), tcsetattr(), ECHO, ICANON, ICRNL, IXTEN, ISIG, IXON, TCSAFLUSH, and also BRKINT, INPCK, ISTRIP, and CS8
#include <unistd.h> // gives us: standard symbolic constants and types

// here we store the original terminal attributes in a global variale
struct termios orig_termios;

void disableRawMode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode(void) {
    // we use tcgetattr() here to read current attributes into a struct
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);

    // struct we create
    struct termios raw = orig_termios;

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

    // here we pass the modified struct to tcsetattr() to write the new terminal attributes back out
    // // TCSAFLUSH argument specifies when to apply the change
    // // // in this case, it waits for all pending output to be written to the terminal, and discards an unread input
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
    enableRawMode();

    char c;
    // while loop will read 1 byte from standard input into char c until there are no more bytes left to read
    // // read returns the number of bytes that it read, and returns 0 when it reaches the end of a file
    // // // terminal by default starts in canonical (cooked) mode where keyboard input is only sent to your program when user hits Enter. We want to change to raw mode by turning off certain flags in the terminal
    while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
        // we now have to manually add in \r for carriage returns because we disabled that above
        if (iscntrl(c)) {
            // if a character is a control characcter, it won't be printable to the screen, but its ASCII value will be printed here
            printf("%d\r\n", c);
        } else {
            // prints out a non-control character and its ASCII value
            printf("%d ('%c')'\r\n", c, c);
        }
    }

    return 0;
}