#include <stdio.h>
#include "lib/shared.h"
extern int alpha_version(void);
extern int beta_version(void);
int main(void)
{
    printf("main=%d alpha=%d beta=%d\n",
        VERSION, alpha_version(), beta_version());
    return 0;
}
