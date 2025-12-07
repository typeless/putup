#include <stdio.h>

extern const char* get_greeting(void);

int main(void) {
    printf("%s\n", get_greeting());
    return 0;
}
