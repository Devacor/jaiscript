// Parse-key + hot-reload-identity micro-benchmarks (parse-avoidance ladder study).
// The Foundry us harness can't resolve this regime, so each
// row here is a dedicated ns measurement: min-of-5 trials, ns/op = best_total/iters.
// Set PARSE_KEY_BENCH_BASELINE to 1 to compile against pre-ladder HEAD (no script_source,
// no identical_redefinitions) for before/after tables.

#define PARSE_KEY_BENCH_BASELINE 0

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace {

// 64-byte parseable comment chunk; macro concatenation builds the sized literals the
// literal lane needs at compile time, and the hash-lane twins reuse the same bytes.
#define PKB_C64 "// 012345678901234567890123456789012345678901234567890123456789\n"
#define PKB_C512 PKB_C64 PKB_C64 PKB_C64 PKB_C64 PKB_C64 PKB_C64 PKB_C64 PKB_C64
#define PKB_C4K PKB_C512 PKB_C512 PKB_C512 PKB_C512 PKB_C512 PKB_C512 PKB_C512 PKB_C512
#define PKB_C32K PKB_C4K PKB_C4K PKB_C4K PKB_C4K PKB_C4K PKB_C4K PKB_C4K PKB_C4K
#define PKB_HEAD "return 7;"
#define PKB_LIT_64 PKB_HEAD PKB_C64
#define PKB_LIT_512 PKB_HEAD PKB_C512
#define PKB_LIT_4K PKB_HEAD PKB_C4K
#define PKB_LIT_32K PKB_HEAD PKB_C32K

template <typename F>
double ns_per_op(size_t iters, F&& fn) {
	double best = 1e18;
	for (int trial = 0; trial < 5; ++trial) {
		auto t0 = std::chrono::steady_clock::now();
		for (size_t i = 0; i < iters; ++i) {
			fn();
		}
		auto t1 = std::chrono::steady_clock::now();
		double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / double(iters);
		if (ns < best) {
			best = ns;
		}
	}
	return best;
}

void row(const char* name, size_t bytes, double ns) {
	std::fprintf(stderr, "  %-46s %7zuB %12.1f ns/op\n", name, bytes, ns);
}

// Realistic "defs" shape: numbered function definitions padded to an exact byte size.
std::string make_defs_script(size_t target) {
	std::string tail = "return tick(42);";
	std::string s = target >= 128 ? "int tick(int frame) { return frame * 3 + 1; }\n"
	                              : "int tick(int f) { return f * 3; }\n";
	int i = 0;
	for (;;) {
		char line[96];
		std::snprintf(line, sizeof(line), "int fn_%04d(int a, int b) { return (a * 31 + b) %% 97 + %d; }\n", i, i);
		if (s.size() + std::strlen(line) + tail.size() + 8 > target) {
			break;
		}
		s += line;
		++i;
	}
	size_t remaining = target - s.size() - tail.size();
	if (remaining >= 4) {
		s += "// ";
		s.append(remaining - 4, 'p');
		s += "\n";
	} else {
		s.append(remaining, ' ');
	}
	s += tail;
	return s;
}

// Status-quo keying replica: reused key buffer + std::hash via unordered_map find,
// byte-for-byte the pre-ladder engine::implementation code path.
struct hash_lane_replica {
	std::unordered_map<std::string, int> map;
	std::string key_buf;

	const std::string& key(const std::string& path, const std::string& source) {
		auto& k = key_buf;
		k.clear();
		k += std::to_string(path.size());
		k += ':';
		k += path;
		k += source;
		return k;
	}
	void insert(const std::string& path, const std::string& source, int v) { map[key(path, source)] = v; }
	int find(const std::string& path, const std::string& source) {
		auto it = map.find(key(path, source));
		return it == map.end() ? -1 : it->second;
	}
};

// Discriminator replica: (lengths + 24 sampled bytes) flat scan + verify memcmp —
// mirrors the ladder's dynamic lane. Worst case: target node scanned last.
struct disc_lane_replica {
	struct node {
		uint64_t path_len, content_len, head, mid, tail;
		std::string path, source;
		int value;
	};
	std::vector<node> nodes;

	static void samples(const std::string& s, uint64_t& head, uint64_t& mid, uint64_t& tail) {
		head = mid = tail = 0;
		size_t n = s.size();
		if (n >= 8) {
			std::memcpy(&head, s.data(), 8);
			std::memcpy(&mid, s.data() + (n / 2) - 4, 8);
			std::memcpy(&tail, s.data() + n - 8, 8);
		} else if (n > 0) {
			std::memcpy(&head, s.data(), n);
		}
	}
	void insert(const std::string& path, const std::string& source, int v) {
		node n{path.size(), source.size(), 0, 0, 0, path, source, v};
		samples(source, n.head, n.mid, n.tail);
		nodes.push_back(std::move(n));
	}
	int find(const std::string& path, const std::string& source) {
		uint64_t head, mid, tail;
		samples(source, head, mid, tail);
		for (auto& n : nodes) {
			if (n.content_len != source.size() || n.path_len != path.size() ||
			    n.head != head || n.mid != mid || n.tail != tail) {
				continue;
			}
			if (std::memcmp(n.path.data(), path.data(), path.size()) != 0 ||
			    std::memcmp(n.source.data(), source.data(), source.size()) != 0) {
				continue;
			}
			return n.value;
		}
		return -1;
	}
};

size_t iters_for(size_t bytes) {
	if (bytes <= 64) return 20000;
	if (bytes <= 512) return 10000;
	if (bytes <= 4096) return 4000;
	return 800;
}

} // namespace

namespace jai::foundry::tests {

class parse_key_bench : public suite {
public:
	parse_key_bench() : suite("Parse Key Bench") {}

	static std::shared_ptr<jai::engine> engine_for(backend_type type) {
		auto eng = jai::foundry::make_default_engine();
		if (type != backend_type::interpreter) {
			eng->set_backend(type);
		}
		return eng;
	}

	static constexpr size_t sizes[4] = {64, 512, 4096, 32768};

	void forge_tests() override {
		static const std::string comment_scripts[4] = {PKB_LIT_64, PKB_LIT_512, PKB_LIT_4K, PKB_LIT_32K};

		test("keying_replicas", [this]() {
			std::fprintf(stderr, "\n[keying replicas: key-build+hash+map-find (status quo) vs disc+scan+memcmp (ladder), 64-entry caches]\n");
			hash_lane_replica hashLane;
			disc_lane_replica discLane;
			std::vector<std::string> targets;
			for (size_t s : sizes) {
				targets.push_back(make_defs_script(s));
			}
			for (int d = 0; d < 60; ++d) {
				hashLane.insert("<script>", make_defs_script(96 + size_t(d) * 71), d);
				discLane.insert("<script>", make_defs_script(96 + size_t(d) * 71), d);
			}
			for (auto& t : targets) {
				hashLane.insert("<script>", t, 1);
				discLane.insert("<script>", t, 1);
			}
			volatile int sink = 0;
			const std::string path = "<script>";
			for (size_t i = 0; i < 4; ++i) {
				const std::string& t = targets[i];
				double hashNs = ns_per_op(iters_for(sizes[i]), [&]() { sink += hashLane.find(path, t); });
				double discNs = ns_per_op(iters_for(sizes[i]), [&]() { sink += discLane.find(path, t); });
				char label[80];
				std::snprintf(label, sizeof(label), "hash-lane keying (defs)");
				row(label, t.size(), hashNs);
				std::snprintf(label, sizeof(label), "disc-lane keying (defs)  [%.1fx]", hashNs / discNs);
				row(label, t.size(), discNs);
			}
			std::fprintf(stderr, "  (sink %d)\n", int(sink));
			check_true(true, "measured");
		});

		test("raw_hash_vs_memcmp", [this]() {
			std::fprintf(stderr, "\n[raw std::hash<string> vs raw memcmp of equal buffers]\n");
			volatile size_t sink = 0;
			// hoisted: std::hash<std::string>{}(a) inline in the nested lambda ICEs MSVC 14.51 p1
			std::hash<std::string> hasher;
			for (size_t s : sizes) {
				std::string a = make_defs_script(s);
				std::string b = a;
				double hashNs = ns_per_op(iters_for(s), [&]() { sink += hasher(a); });
				double cmpNs = ns_per_op(iters_for(s), [&]() { sink += size_t(std::memcmp(a.data(), b.data(), a.size())); });
				char label[80];
				std::snprintf(label, sizeof(label), "std::hash");
				row(label, s, hashNs);
				std::snprintf(label, sizeof(label), "memcmp (equal)  [%.1fx]", hashNs / cmpNs);
				row(label, s, cmpNs);
			}
			std::fprintf(stderr, "  (sink %zu)\n", size_t(sink));
			check_true(true, "measured");
		});

		for (auto backend : {backend_type::interpreter, backend_type::vm}) {
			const char* backendName = backend == backend_type::vm ? "vm" : "interpreter";

			test(std::string("execute_hit_comment_") + backendName, [this, backend, backendName]() {
				std::fprintf(stderr, "\n[execute(str) cache-hit, comment shape (execution ~constant: keying isolates in the slope), %s]\n", backendName);
				auto eng = engine_for(backend);
				for (size_t i = 0; i < 4; ++i) {
					const std::string& script = comment_scripts[i];
					eng->execute(script);
					double ns = ns_per_op(iters_for(sizes[i]), [&]() { eng->execute(script); });
					row("execute(str) hit (comment)", script.size(), ns);
				}
				check_true(true, "measured");
			});

			test(std::string("execute_hit_defs_") + backendName, [this, backend, backendName]() {
				std::fprintf(stderr, "\n[execute(str) cache-hit, defs shape (realistic: N function defs re-run per execute), %s]\n", backendName);
				auto eng = engine_for(backend);
				for (size_t i = 0; i < 4; ++i) {
					std::string script = make_defs_script(sizes[i]);
					eng->execute(script);
					double ns = ns_per_op(iters_for(sizes[i]) / 4 + 1, [&]() { eng->execute(script); });
					row("execute(str) hit (defs)", script.size(), ns);
				}
				check_true(true, "measured");
			});

#if !PARSE_KEY_BENCH_BASELINE
			test(std::string("execute_literal_hit_") + backendName, [this, backend, backendName]() {
				std::fprintf(stderr, "\n[execute(script_source) literal-lane hit, comment shape, %s]\n", backendName);
				auto eng = engine_for(backend);
				eng->execute(script_source(PKB_LIT_64));
				double ns64 = ns_per_op(iters_for(64), [&]() { eng->execute(script_source(PKB_LIT_64)); });
				row("execute(script_source) hit", sizeof(PKB_LIT_64) - 1, ns64);
				eng->execute(script_source(PKB_LIT_512));
				double ns512 = ns_per_op(iters_for(512), [&]() { eng->execute(script_source(PKB_LIT_512)); });
				row("execute(script_source) hit", sizeof(PKB_LIT_512) - 1, ns512);
				eng->execute(script_source(PKB_LIT_4K));
				double ns4k = ns_per_op(iters_for(4096), [&]() { eng->execute(script_source(PKB_LIT_4K)); });
				row("execute(script_source) hit", sizeof(PKB_LIT_4K) - 1, ns4k);
				eng->execute(script_source(PKB_LIT_32K));
				double ns32k = ns_per_op(iters_for(32768), [&]() { eng->execute(script_source(PKB_LIT_32K)); });
				row("execute(script_source) hit", sizeof(PKB_LIT_32K) - 1, ns32k);
				check_true(true, "measured");
			});
#endif

			test(std::string("execute_file_hit_") + backendName, [this, backend, backendName]() {
				std::fprintf(stderr, "\n[execute_file re-execute (warm) + raw component costs, %s]\n", backendName);
				namespace fs = std::filesystem;
				fs::path dir = jai::foundry::test_temp_directory() / "jai_parse_key_bench";
				fs::create_directories(dir);
				auto eng = engine_for(backend);
				for (size_t s : {size_t(4096), size_t(32768)}) {
					fs::path file = dir / ("probe_" + std::to_string(s) + ".jai");
					std::string script = make_defs_script(s);
					{
						std::ofstream out(file, std::ios::binary | std::ios::trunc);
						out.write(script.data(), std::streamsize(script.size()));
					}
					std::string pathStr = file.string();
					eng->execute_file(pathStr);
					eng->execute_file(pathStr);   // second call settles jaibite sibling + verify states
					double ns = ns_per_op(600, [&]() { eng->execute_file(pathStr); });
					row("execute_file warm", s, ns);

					volatile size_t sink = 0;
					double readNs = ns_per_op(600, [&]() {
						std::ifstream in(pathStr);
						std::stringstream buffer;
						buffer << in.rdbuf();
						sink += buffer.str().size();
					});
					row("raw read (ifstream+stringstream)", s, readNs);
					double statNs = ns_per_op(2000, [&]() {
						std::error_code ec1, ec2;
						auto t = fs::last_write_time(pathStr, ec1);
						(void)t;
						sink += size_t(fs::file_size(pathStr, ec2));
					});
					row("fs stat pair (mtime+size)", s, statNs);
					std::fprintf(stderr, "  (sink %zu)\n", size_t(sink));
				}
				std::error_code cleanupEc;
				fs::remove_all(dir, cleanupEc);
				check_true(true, "measured");
			});

			test(std::string("cold_unique_strings_") + backendName, [this, backend, backendName]() {
				std::fprintf(stderr, "\n[cold execute of never-seen 4KB strings (keying miss + full parse), %s]\n", backendName);
				auto eng = engine_for(backend);
				std::string base = make_defs_script(4096 - 24);
				int counter = 0;
				double ns = ns_per_op(120, [&]() {
					char suffix[32];
					std::snprintf(suffix, sizeof(suffix), "// u%016d\n", counter++);
					eng->execute(base + suffix);
				});
				row("execute(str) cold (unique 4KB)", 4096, ns);
				check_true(true, "measured");
			});

			test(std::string("reload_identity_") + backendName, [this, backend, backendName]() {
				std::fprintf(stderr, "\n[identical class redefinition per re-execute (hot-reload identity), %s]\n", backendName);
				auto eng = engine_for(backend);

				std::string smallClass =
					"class Calc {\n"
					"    auto result = 0.0;\n"
					"    auto memory = 0.0;\n"
					"    void add(x) { result = result + x; }\n"
					"    void mul(x) { result = result * x; }\n"
					"    void store() { memory = result; }\n"
					"    void recall() { result = memory; }\n"
					"}\n";
				std::string largeClass = "class Mob {\n";
				for (int f = 0; f < 12; ++f) {
					largeClass += "    auto field_" + std::to_string(f) + " = " + std::to_string(f) + ".0;\n";
				}
				for (int m = 0; m < 24; ++m) {
					largeClass += "    void method_" + std::to_string(m) + "(x) { field_0 = field_0 + x * " +
					              std::to_string(m + 1) + "; }\n";
				}
				largeClass += "}\n";

				eng->execute(smallClass);
				eng->execute("auto calcs = []; for (int i = 0; i < 64; ++i) { calcs.push(Calc()); }");
				eng->execute(smallClass);   // seeds the structural key (first redefinition)
				double smallNs = ns_per_op(400, [&]() { eng->execute(smallClass); });
				row("identical reload: small class, 64 instances", smallClass.size(), smallNs);

				eng->execute(largeClass);
				eng->execute("auto mobs = []; for (int i = 0; i < 1000; ++i) { mobs.push(Mob()); }");
				eng->execute(largeClass);
				double largeNs = ns_per_op(200, [&]() { eng->execute(largeClass); });
				row("identical reload: large class, 1000 instances", largeClass.size(), largeNs);

#if !PARSE_KEY_BENCH_BASELINE
				auto def = detail::engine_access::get_class_definition(*eng, "Calc");
				check_not_null(def.get(), "Calc definition");
				check_true(def->identical_redefinitions() > 0, "structural fast path engaged during bench");
				std::fprintf(stderr, "  (Calc identical_redefinitions = %zu)\n", def->identical_redefinitions());
#endif
				check_true(true, "measured");
			});
		}
	}
};

} // namespace jai::foundry::tests

using parse_key_bench = jai::foundry::tests::parse_key_bench;
FOUNDRY_REGISTER(parse_key_bench)
