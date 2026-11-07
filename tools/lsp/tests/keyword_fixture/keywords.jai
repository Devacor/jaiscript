// keyword-named members, declared and reached qualified
class Promise {
	int default = 7;
	int catch(int code) { return code + this.default; }
	string new() { return "fresh"; }
}

class Retry : Promise {
	override int catch(int code) { return super::catch(code) * 2; }
}

namespace spider {
	int import = 3;
	string catch(string url) { return "fetched " + url; }
}

enum Mode { default, for }

Promise p;
Retry r;
int settled = p.catch(1) + r.catch(2) + p.default;
string page = spider::catch("a.com") + p.new();
int imports = spider::import;
int m = Mode::for;
print(settled);
