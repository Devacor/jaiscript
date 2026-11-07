#pragma once

#ifndef __JAISCRIPT_CORE_JAIBITE_HPP__
#define __JAISCRIPT_CORE_JAIBITE_HPP__

#include <jaiscript/detail/type_checker.hpp>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

namespace jai {

    class engine;
    class declaration;
    using declaration_ptr = std::shared_ptr<declaration>;
    class script_value;

    // Canonical file extension for saved bites. save()/jaibite_load() take explicit
    // paths (the convention is not enforced); the format keeps its "JBIT" magic
    // regardless of filename.
    inline constexpr const char* k_jaibite_extension = ".jaibite";
    // The folder the automatic disk cache keeps beside each source folder (engine::jaibite_cache_path).
    inline constexpr const char* k_jaibite_cache_folder = ".cache";

    // Engine-bound pre-parsed script: engine->jaibite("...") lexes and parses once,
    // then execute() re-runs without re-lexing/parsing (and the vm backend caches its
    // compiled chunk in compiled_, so re-execution is pure dispatch). Never share a
    // jaibite across engines — interned symbol IDs are engine-specific; to move a bite
    // between engines go through save()/engine::jaibite_load(), which relocates symbols.
    class jaibite {
    public:
        jaibite() = default;

        // Runs on the owning engine; throws script_exception if the engine is gone.
        script_value execute();

        bool valid() const { return !declarations_.empty(); }

        // Serialize the parsed AST (never the compiled chunk — a loaded bite recompiles
        // lazily like a fresh one). Throws jai::runtime_error on IO failure or if the
        // owning engine is gone. Load side: engine::jaibite_load / jaibite_load_bytes.
        void save(const std::string& path) const;
        std::vector<uint8_t> save_bytes() const;  // byte-level, for host VFS routing

        // True when the loading engine's registered classes/functions differ from the
        // saving engine's. Advisory only — the bite still loads and runs (the script may
        // not touch the missing registrations); errors surface at execute() if it does.
        bool registration_mismatch() const { return registration_mismatch_; }

        // Static-check state. A created or executed bite
        // carries its diagnostics; checked_clean() means it was checked and has no
        // error-severity diagnostics. save() stamps that flag beside the registration
        // fingerprint and the names the check resolved outside the bite; jaibite_load trusts the stamp only when the fingerprint
        // matches and those names resolve alike in the loading engine, otherwise the bite is checked at load (a bite is
        // checked ONCE — later registrations don't re-trigger; re-create the bite to re-check).
        bool checked_clean() const { return check_state_ == check_state::clean; }
        const check_report& check_diagnostics() const { return check_report_; }

    private:
        friend class engine;
        jaibite(std::weak_ptr<engine> owner, std::vector<declaration_ptr> declarations)
            : engine_(std::move(owner)), declarations_(std::move(declarations)) {}

        enum class check_state : uint8_t { unchecked, clean, flagged };

        std::weak_ptr<engine> engine_;
        std::vector<declaration_ptr> declarations_;
        std::shared_ptr<void> compiled_;   // backend-owned compiled artifact (vm: chunk)
        bool registration_mismatch_ = false;
        check_state check_state_ = check_state::unchecked;
        check_report check_report_;
        detail::jaibite_context context_;   // a clean check's outside names and their key, saved with the stamp
        bool stamp_loaded_ = false;         // a loaded stamp, held against the loading engine at its first check
        bool context_recorded_ = false;     // context_ is the one a clean check keyed: a later check keeps it (a load never rewrites)
        uint64_t proof_epoch_ = UINT64_MAX;   // UINT64_MAX = nodes carry no proofs
        bool autos_typed_ = false;   // the first proof pass types the auto slots
        uint32_t unit_ = 0;   // shadowing unit, numbered at the first execute
        std::shared_ptr<void> lowering_;   // detail::aggregate_lowering_state (engine::aggregate_lowering)
        declaration_ptr reload_hook_;      // the source's hot_reload_migrate, kept off the program and saved with it; a bite runs no hook
    };

} // namespace jai

#endif // __JAISCRIPT_CORE_JAIBITE_HPP__
