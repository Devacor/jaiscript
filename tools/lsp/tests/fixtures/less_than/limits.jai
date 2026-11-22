// the names use.jai reads after a name and a <: variables, a base class with fields, an enum, a generic
int floor_lo = 1;
int floor_hi = 5;
class Range { int lo = 1; int hi = 5; }
class Limits { static int cap = 9; }
enum Level { low, mid, high }
namespace bounds { int inner = 2; }
T first_of<T>(T x) { return x; }
class Shelf<T> { T item; static int size() { return 4; } }
