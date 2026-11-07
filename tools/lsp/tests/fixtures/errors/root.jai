import "typed_leaf.jai";
import "broken_leaf.jai";

int answer() {
	return helper() + LEAF_VALUE + typed_value();
}

int wrong() {
	return "not a number";
}

void calls() {
	int n = answer();
	missing_function(n);
}
