// One half of a namespace two files declare: the members the other half names bare.

namespace shop {
	class Item { int price = 3; }
	int tax(int p) { return p + 1; }
	int base = 10;
	enum Kind { plain, fancy }
	namespace stock { int count() { return 2; } }
}
