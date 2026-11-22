import "limits.jai";

class Pet { int v = 7; }
bool both(bool a, bool b) { return a && b; }

// '<' after another file's variable, an inherited field, a static, an enum value and a namespace's variable compares
class Narrow : Range {
	bool inside() { return both(lo < hi, hi > (lo)) && both(below < hi, hi > (below)); }
	int below = 0;
}
bool floors() { return both(floor_lo < floor_hi, floor_hi > (floor_lo)); }
bool caps() { return both(Limits::cap < later, bounds::inner > (floor_lo)) && both(Level::low < Level::high, Level::high > (Level::low)); }
bool members() { Range r; return both(r.lo < floor_hi, floor_hi > (r.lo)); }
// and after another file's generic it opens type arguments
int made() { return first_of<Pet>(Pet()).v + Shelf<Pet>::size(); }
int later = 12;

print(floors() && caps() && members() && Narrow().inside(), made());
