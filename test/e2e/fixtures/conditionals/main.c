#include <stdio.h>

int main(void)
{
#ifdef USE_GREETING
    printf("Hello, World!\n");
#else
    printf("Goodbye, World!\n");
#endif

#ifdef USE_EXTRA
    printf("Extra enabled\n");
#endif

    return 0;
}
