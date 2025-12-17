#include <stdio.h>

int main()
{
#ifdef DEBUG
    printf("Debug mode\n");
#else
    printf("Release mode\n");
#endif
    return 0;
}
