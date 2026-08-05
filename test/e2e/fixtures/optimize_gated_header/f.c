#ifdef __OPTIMIZE__
#include "opt.h"
#else
#include "noopt.h"
#endif
#include <stdio.h>

int main(void)
{
    printf("%d\n", VALUE);
    return 0;
}
