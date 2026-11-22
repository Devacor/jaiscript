// two programs that each declare largest<T>: neither sees the other's
T largest<T>(array<T> xs) { T best = xs[0]; return best; }
int big = largest([3, 4]);
print(big);
