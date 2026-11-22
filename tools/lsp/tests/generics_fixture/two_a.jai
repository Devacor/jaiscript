// two programs that each declare largest<T>: neither sees the other's
T largest<T>(array<T> xs) { T best = xs[0]; for (auto x : xs) { if (x > best) { best = x; } } return best; }
int big = largest([1, 2]);
print(big);
