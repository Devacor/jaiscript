import "shelf.jai";
Stack<int> numbers;
numbers.push(3);
int big = largest([1, 2]);
shop::Shelf<string> s;
s.items.push("tee");
string one = shop::first(["a"]);
shop::Plain q;
int a = 1;
int b = 2;
int c = 3;
int d = 4;
bool both(bool x, bool y) { return x && y; }
print(both(a < b, c > (d)), big, one, s.count(), numbers.size());
