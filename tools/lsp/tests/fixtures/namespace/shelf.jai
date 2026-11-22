import "shop.jai";

namespace shop {
	int shelf_total() {
		Item stocked;
		return tax(stocked.price) + base;
	}
	Kind shelf_kind() { return Kind::fancy; }
	int shelf_count() { return stock::count(); }
}

print(shop::shelf_total());
