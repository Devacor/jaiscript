// the twin of generic.jai
class Entity { int id = 0; void hit() {} }
class Stack {
    array<int> items;
    void push(int item) { items.push(item); }
    int pop() { int last = items[items.size() - 1]; items.pop(); return last; }
    int size() { return items.size(); }
    string fold(string seed) { return seed; }
}
class Pair { string first; Stack second; }
int largest(array<int> xs) { int best = xs[0]; for (auto x : xs) { if (x > best) { best = x; } } return best; }
int id_of(Entity e) { return e.id; }
Stack numbers;
numbers.push(3);
int top = numbers.pop();
Pair p;
p.second.push(1);
int big = largest([1, 2]);
int big2 = largest([1, 2]);
auto f = numbers.fold("s");
int who = id_of(Entity());
