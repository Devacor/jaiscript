// generics another unit imports, at program scope and in a namespace
class Stack<T> {
    array<T> items;
    void push(T item) { items.push(item); }
    int size() { return items.size(); }
}
T largest<T>(array<T> xs) { T best = xs[0]; for (auto x : xs) { if (x > best) { best = x; } } return best; }
namespace shop {
    class Shelf<T> { array<T> items; int count() { return items.size(); } }
    T first<T>(array<T> xs) { return xs[0]; }
    class Plain { int v = 0; }
}
