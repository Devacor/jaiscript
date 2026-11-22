// generic code beside plain.jai, its twin written without type parameters
class Entity { int id = 0; void hit() {} }
class Stack<T> {
    array<T> items;
    void push(T item) { items.push(item); }
    T pop() { T last = items[items.size() - 1]; items.pop(); return last; }
    int size() { return items.size(); }
    U fold<U>(U seed) { return seed; }
}
class Pair<K, V> { K first; V second; }
T largest<T>(array<T> xs) { T best = xs[0]; for (auto x : xs) { if (x > best) { best = x; } } return best; }
int id_of<T : Entity>(T e) { return e.id; }
Stack<int> numbers;
numbers.push(3);
int top = numbers.pop();
Pair<string, Stack<int>> p;
p.second.push(1);
int big = largest([1, 2]);
int big2 = largest<int>([1, 2]);
auto f = numbers.fold<string>("s");
int who = id_of(Entity());
