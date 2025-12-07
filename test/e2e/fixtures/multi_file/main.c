#include <stdio.h>

extern int add(int a, int b);
extern int multiply(int a, int b);

int main(void) {
    printf("add(2,3) = %d\n", add(2, 3));
    printf("multiply(4,5) = %d\n", multiply(4, 5));
    return 0;
}
