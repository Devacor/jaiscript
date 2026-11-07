#pragma once

// Loader-only byte mutation fuzzing. Mutated trees are deliberately NOT executed:
// execution fuzzing and its result/print oracle remain in fuzz_harness.hpp.
#include "fuzz_generator.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace jai::fuzz {

	struct jaibite_mutant {
		uint64_t seed = 0;
		uint64_t source_seed = 0;
		std::string source;
		std::vector<uint8_t> original;
		std::vector<uint8_t> bytes;
		size_t offset = 0;
		std::string operation;
	};

	inline std::shared_ptr<engine> mutation_engine(bool use_vm, bool proofs = true,
	                                             bool param_alias = true) {
		auto result = engine::make();
		result->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
		result->type_proofs(proofs);
		result->parameter_alias(param_alias);
		result->local_alias(param_alias);
		result->execution_budget(0.25);
		result->memory_cap(64 * 1024 * 1024);
		return result;
	}

	inline jaibite_mutant make_jaibite_mutant(uint64_t seed, int max_depth = 4,
	                                         bool proofs = true, bool param_alias = true) {
		jaibite_mutant result;
		result.seed = seed;
		// Find a genuinely serializable generator program; parse-invalid candidates
		// do not count toward the requested number of byte mutants. First segments
		// stand alone; later hot-reload segments can require prior execution.
		for (uint64_t attempt = 0; attempt < 128; ++attempt) {
			result.source_seed = attempt == 0 ? seed : mix64(seed + attempt);
			program p = generate_program(result.source_seed, max_depth);
			if (p.segments.empty()) continue;
			auto author = mutation_engine(false, proofs, param_alias);
			// Semantic errors in generator programs are irrelevant to structural loading: the unit's [[warn_mode]] region is
			// serialized with it, so the loader's check walks every bite and a mutant keeps the region unless a mutation hits it
			const std::string source = k_warn_region_prefix + p.segments.front();
			try {
				auto bite = author->jaibite(source);
				if (!bite.valid()) continue;
				result.original = bite.save_bytes();
				result.source = source;
				break;
			} catch (const jai::parse_error&) {
				// Generator parse rejection, not a loader verdict.
			} catch (const jai::static_check_error&) {
				// A load error the region does not demote: not a loader verdict either.
			}
		}
		if (result.original.empty()) {
			throw std::runtime_error("mutation corpus: no serializable program after 128 candidates");
		}
		result.bytes = result.original;
		fuzz_rng rng(mix64(seed ^ 0xC9B7E3F1524068ADULL));
		// Most cases retain the envelope so they reach the decoded-tree validator.
		// Every ninth case can hit the magic/version/fingerprint/string-table header.
		const size_t begin = seed % 9 == 0 ? 0 : std::min<size_t>(20, result.bytes.size() - 1);
		result.offset = begin + static_cast<size_t>(rng.next() % (result.bytes.size() - begin));
		auto at = result.bytes.begin() + static_cast<std::ptrdiff_t>(result.offset);
		switch (seed % 8) {
			case 0:
				result.operation = "bit-flip";
				result.bytes[result.offset] ^= uint8_t(1U << rng.below(8));
				break;
			case 1: {
				result.operation = "byte-overwrite";
				uint8_t replacement = uint8_t(rng.next());
				if (replacement == result.bytes[result.offset]) replacement ^= 0xFF;
				result.bytes[result.offset] = replacement;
				break;
			}
			case 2: {
				result.operation = "erase-span";
				const size_t count = std::min<size_t>(1 + rng.below(16), result.bytes.size() - result.offset);
				result.bytes.erase(at, at + static_cast<std::ptrdiff_t>(count));
				break;
			}
			case 3: {
				result.operation = "insert-span";
				std::vector<uint8_t> extra(1 + rng.below(16));
				for (auto& value : extra) value = uint8_t(rng.next());
				result.bytes.insert(at, extra.begin(), extra.end());
				break;
			}
			case 4:
				result.operation = "truncate";
				result.bytes.resize(result.offset);
				break;
			case 5: {
				result.operation = "varint-continuations";
				const size_t count = std::min<size_t>(10, result.bytes.size() - result.offset);
				std::fill_n(at, count, uint8_t(0xFF));
				break;
			}
			case 6: {
				result.operation = "zero-span";
				const size_t count = std::min<size_t>(1 + rng.below(8), result.bytes.size() - result.offset);
				std::fill_n(at, count, uint8_t(0));
				break;
			}
			case 7:
				result.operation = "append-tail";
				result.offset = result.bytes.size();
				result.bytes.push_back(uint8_t(rng.next()));
				break;
		}
		// A zero/0xff streak can already match the selected span: never count a no-op.
		if (result.bytes == result.original) result.bytes[result.offset] ^= 1;
		return result;
	}

	struct jaibite_load_outcome {
		bool accepted = false;
		bool alive = false;
		bool stable_bytes = true;
		std::string loader_error;
		std::string unexpected_error;
	};

	inline jaibite_load_outcome load_mutant_bytes(const std::vector<uint8_t>& bytes,
	                                              bool use_vm, bool require_stable_bytes,
	                                              bool proofs = true, bool param_alias = true) {
		jaibite_load_outcome result;
		auto host = mutation_engine(use_vm, proofs, param_alias);
		try {
			auto bite = host->jaibite_load_bytes(bytes);
			if (require_stable_bytes) result.stable_bytes = bite.save_bytes() == bytes;
			result.accepted = true;
		} catch (const jai::static_check_error& error) {
			result.loader_error = error.what();   // a mutation that removed or changed the [[warn_mode]] region: the checker's verdict
		} catch (const jai::runtime_error& error) {
			result.loader_error = error.what();
			if (result.loader_error.rfind("jaibite: ", 0) != 0) {
				result.unexpected_error = "non-loader JaiScript error: " + result.loader_error;
			}
		} catch (const std::exception& error) {
			// In particular, std::bad_alloc is NOT a clean loader rejection.
			result.unexpected_error = "native exception: " + std::string(error.what());
		} catch (...) {
			result.unexpected_error = "non-standard exception";
		}
		try {
			result.alive = host->execute("int mutation_recovery_value = 2; mutation_recovery_value;").as_int() == 2;
		} catch (...) {
			result.alive = false;
		}
		return result;
	}

	struct jaibite_mutation_result {
		bool ok = true;
		bool rejected = false;
		std::string report;
	};

	inline jaibite_mutation_result compare_jaibite_mutant(const jaibite_mutant& mutant,
	                                                     bool proofs = true, bool param_alias = true) {
		jaibite_mutation_result result;
		auto problem = [&](const std::string& message) {
			result.ok = false;
			result.report += message + "\n";
		};
		for (bool use_vm : { false, true }) {
			const auto original = load_mutant_bytes(mutant.original, use_vm, true, proofs, param_alias);
			const std::string backend = use_vm ? "vm" : "interp";
			if (!original.accepted || !original.unexpected_error.empty()) {
				problem(backend + " rejected valid original: " + original.loader_error + original.unexpected_error);
			}
			if (!original.stable_bytes) problem(backend + " changed valid original bytes");
			if (!original.alive) problem(backend + " unusable after loading valid original");
		}
		if (!result.ok) return result;
		const auto interp = load_mutant_bytes(mutant.bytes, false, false, proofs, param_alias);
		const auto vm = load_mutant_bytes(mutant.bytes, true, false, proofs, param_alias);
		if (!interp.unexpected_error.empty()) problem("interp " + interp.unexpected_error);
		if (!vm.unexpected_error.empty()) problem("vm " + vm.unexpected_error);
		if (!interp.alive) problem("interp unusable after mutant load");
		if (!vm.alive) problem("vm unusable after mutant load");
		if (interp.accepted != vm.accepted) problem("loader acceptance differs between backends");
		if (interp.loader_error != vm.loader_error) {
			problem("loader error text differs:\n  interp: " + interp.loader_error + "\n  vm: " + vm.loader_error);
		}
		result.rejected = !interp.accepted && !vm.accepted;
		return result;
	}

} // namespace jai::fuzz
