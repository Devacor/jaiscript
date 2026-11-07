#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class atomic_tests : public suite {
public:
	atomic_tests() : suite("Atomics") {}

	std::shared_ptr<engine> make_atomic_engine() {
		auto eng = make_engine();
		stdlib::register_all(eng);
		stdlib::register_atomics(eng);
		return eng;
	}

	static std::string raised(engine& a_engine, const std::string& a_code) {
		return a_engine.execute("string raised = \"no error\"; try { " + a_code + " } catch (e) { raised = e; } raised").as<std::string>();
	}

	void forge_tests() override {
		test("atomic_int_runs_each_operation_as_cpp_does", [this]() {
			std::atomic<int64_t> hits{5};
			auto eng = make_atomic_engine();
			eng->add_global_ref("hits", hits);
			check_eq(std::string("5 5 9 9 8 8 6 6 7 5 5 10 9 1"), eng->execute(R"(
				string out = "";
				out += to_string(hits.load()) + " " + to_string(hits.fetch_add(4)) + " " + to_string(hits.load());
				out += " " + to_string(hits.fetch_sub(1)) + " " + to_string(hits.load());
				out += " " + to_string(hits--) + " " + to_string(--hits) + " " + to_string(hits++) + " " + to_string(hits);
				out += " " + to_string(hits -= 2) + " " + to_string(hits);
				out += " " + to_string(hits += 5);
				hits = 9;
				out += " " + to_string(hits) + " " + to_string(++hits - 9);
				out;
			)").as<std::string>());
			check_eq(std::string("10 true|10 false 20|20|6 7 6"), eng->execute(R"(
				int seen = 10;
				string out = to_string(hits.exchange(seen)) + " " + to_string(hits.compare_exchange(seen, 20));
				int stale = 10;
				bool swapped = hits.compare_exchange(stale, 30);
				out += "|" + to_string(seen) + " " + to_string(swapped) + " " + to_string(stale);
				int n = hits;
				out += "|" + to_string(n);
				hits.store(6);
				out += "|" + to_string(hits.fetch_or(1)) + " " + to_string(hits.fetch_and(6)) + " " + to_string(hits.fetch_xor(6));
				out;
			)").as<std::string>());
			check_eq(true, eng->execute("hits == 0 && !(hits == 1) && !hits && hits != 2").as_bool());
			check_eq((int64_t)0, hits.load());
			hits.store(std::numeric_limits<int64_t>::max());
			check_eq(std::string("Integer overflow in '+='"), raised(*eng, "hits += 1;"));
			check_eq(std::string("Integer overflow in '++'"), raised(*eng, "hits++;"));
			check_eq(std::string("Integer overflow in '+'"), raised(*eng, "hits.fetch_add(1);"));
			check_eq(std::numeric_limits<int64_t>::max(), hits.load());
		});

		test("atomic_float_and_atomic_bool_run_their_operations", [this]() {
			std::atomic<double> load{1.5};
			std::atomic<bool> maintenance{false};
			auto eng = make_atomic_engine();
			eng->add_global_ref("load_avg", load);
			eng->add_global_ref("maintenance", maintenance);
			check_eq(std::string("1.5 1.5 3.0 2.5 2.5 2.0 4.0 true|false false true true false"), eng->execute(R"(
				string out = to_string(load_avg) + " " + to_string(load_avg.fetch_add(1.5)) + " " + to_string(load_avg.load());
				out += " " + to_string(load_avg -= 0.5) + " " + to_string(load_avg.fetch_sub(0.5)) + " " + to_string(load_avg.exchange(4.0));
				float f = load_avg;
				out += " " + to_string(f) + " " + to_string(load_avg == 4.0);
				out += "|" + to_string(maintenance) + " " + to_string(maintenance.exchange(true)) + " " + to_string(maintenance.load());
				bool expected = true;
				out += " " + to_string(maintenance.compare_exchange(expected, false)) + " " + to_string(maintenance == true);
				out;
			)").as<std::string>());
			check_eq(false, maintenance.load());
			check_eq(std::string("Cannot apply '++' to atomic<float>"), raised(*eng, "load_avg++;"));
		});

		test("an_atomic_is_never_copied", [this]() {
			std::atomic<int64_t> hits{0};
			auto eng = make_atomic_engine();
			eng->add_global_ref("hits", hits);
			for (const char* copy : {"auto c = hits;", "var v = hits;", "void take(atomic<int> a_hits) {} take(hits);"}) {
				const std::string text = raised(*eng, copy);
				check_true(text.find("Cannot copy C++ object of type 'atomic<int>'") != std::string::npos, std::string(copy) + " -> " + text);
			}
			eng->execute("atomic<int>& alias = hits; alias += 3; void bump(atomic<int>& a_hits) { a_hits++; } bump(hits); bump(alias);");
			check_eq((int64_t)5, hits.load());
		});

		test("engines_on_threads_share_one_atomic_by_reference", [this]() {
			std::atomic<int64_t> hits{0};
			std::atomic<double> weight{0.0};
			std::atomic<bool> seen{false};
			constexpr int k_threads = 8;
			constexpr int k_rounds = 2000;
			std::vector<std::string> errors(k_threads);
			std::atomic<int> ready{0};
			std::vector<std::thread> threads;
			for (int t = 0; t < k_threads; ++t) {
				threads.emplace_back([&, t]() {
					try {
						auto eng = make_atomic_engine();
						eng->add_global_ref("hits", hits);
						eng->add_global_ref("weight", weight);
						eng->add_global_ref("seen", seen);
						++ready;
						while (ready.load() < k_threads) { std::this_thread::yield(); }
						eng->execute("for (int i = 0; i < " + std::to_string(k_rounds) + "; ++i) { hits++; hits += 2; hits.fetch_sub(1); weight += 0.5; seen = true; }");
					} catch (const std::exception& e) {
						errors[t] = e.what();
					}
				});
			}
			for (auto& thread : threads) { thread.join(); }
			for (const auto& error : errors) { check_eq(std::string(), error); }
			check_eq((int64_t)k_threads * k_rounds * 2, hits.load());
			check_eq(0.5 * k_threads * k_rounds, weight.load());
			check_eq(true, seen.load());
		});
	}
};

}

using atomic_tests = jai::foundry::tests::atomic_tests;
FOUNDRY_REGISTER(atomic_tests)
