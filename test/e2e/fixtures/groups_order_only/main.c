#include <stdio.h>
// config.h is included via -include flag, defines CONFIG_VALUE

int main(void)
{
    printf("CONFIG_VALUE=%d\n", CONFIG_VALUE);
    return 0;
}
