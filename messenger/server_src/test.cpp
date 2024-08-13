#include <iostream>

int mod_pow(int x, int n, int p)
{
    if (n == 0)
        return 1;
    if (n % 2 == 0) {
        int a = mod_pow(x, n / 2, p);
        return (a * a) % p;
    } else {
        int a = mod_pow(x, (n - 1) / 2, p);
        return (x * a * a) % p;
    }
}

int main() {
    int p = 7;
    int stop = 1000;
    for (int i = 0; i < stop; i++) {
        if (mod_pow(2, i, p) == i % p)
            std::cout<<i<<": "<<i%p<<'\n';
    }
}