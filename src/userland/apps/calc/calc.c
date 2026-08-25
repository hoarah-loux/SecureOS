#include <stdio.h>

int main(void) {
    int a = 2;
    int b = 5;

    int sum = a + b;
    printf("The sum of %d and %d is %d\n", a, b, sum);
    
    char* msg = "Assaf is a good programmer";
    char* msg2 = "Assaf is also a good person";
    char* msg3 = "Assaf is a good programmer and a good person";

    printf("%s\n", msg);
    printf("%s\n", msg2);
    printf("%s\n", msg3);

    return 0;
}