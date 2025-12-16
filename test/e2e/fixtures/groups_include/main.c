#include <stdio.h>
// config.h is included via -include flag

int main(void)
{
    printf("CONFIG_VALUE=%d\n", CONFIG_VALUE);
    return 0;
}
