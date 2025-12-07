extern int add(int a, int b);
extern int mul(int a, int b);
extern void print_result(const char *op, int a, int b, int result);

int main(void)
{
    print_result("+", 3, 4, add(3, 4));
    print_result("*", 3, 4, mul(3, 4));
    return 0;
}
