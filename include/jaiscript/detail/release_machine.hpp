#pragma once

#ifndef __JAISCRIPT_DETAIL_RELEASE_MACHINE_HPP__
#define __JAISCRIPT_DETAIL_RELEASE_MACHINE_HPP__

// A dying object's members past a native destruction depth (engine::release_recursion_limit) die on this engine-owned
// stack instead of the native one, in exactly the order recursion would kill them: the stack's top dies first and
// everything one release step drops natively is deferred onto it in drop order, so a step's children die depth first
// before the next step. A dying instance whose chain declares a destructor is a frame: its levels' bodies and own fields
// run as actions, as class_definition::destroy_levels runs them. Script entered while a step drops values (a host
// destructor's callback) first finishes what the step deferred (release_sync), so script never sees a deferred death.

#include <jaiscript/core/value.hpp>
#include <memory>
#include <vector>
#include <cstdint>

namespace jai {

class class_instance;
class class_definition;

namespace detail {

struct release_action {
    const class_definition* level = nullptr;   // null: a field action
    uint64_t field = 0;
};

struct release_frame {
    class_instance* instance = nullptr;
    std::shared_ptr<class_definition> owner;   // the allocation's class (its deleter's capture)
    weaker_ptr<class_instance> allocation;  // keeps the instance's memory (it goes with its control block) until the frame ends
    const class_definition* root = nullptr;    // the level destroy_levels started from
    const class_definition* kept = nullptr;
    const class_definition* constructed = nullptr;   // a half-built instance's deepest completed level (null: every level)
    std::vector<release_action> actions;       // back = next
    bool delete_at_end = false;
};

struct release_machine {
    static constexpr uint32_t k_no_frame = UINT32_MAX;
    struct entry {
        script_value value;
        uint32_t frame = k_no_frame;
    };
    std::vector<entry> stack;
    std::vector<release_frame> frames;
    std::vector<uint32_t> free_frames;
    // destroy_levels' action lists and destructor-call argument lists, kept with their capacity (each nested destruction takes its own)
    std::vector<std::vector<release_action>> spare_actions;
    std::vector<std::vector<script_value>> spare_arguments;
    size_t step_mark = 0;
    uint32_t depth = 0;
    uint32_t depth_limit = 64;
    bool deferring = false;
};

// Runs the stack down to `mark`
void release_drive(engine& eng, release_machine& m, size_t mark);
// A dying instance's frame (deleter of a destructor class): runs now unless a step is deferring
void release_instance_frame(engine& eng, release_machine& m, class_instance* instance, std::shared_ptr<class_definition> owner,
                            bool run_chain, const class_definition* constructed);
// Before script runs inside a deferring step: the step's deferred deaths happen first, and drops inside the script are
// immediate until the guard ends
class release_sync {
public:
    explicit release_sync(engine* eng) noexcept;
    ~release_sync();
    release_sync(const release_sync&) = delete;
    release_sync& operator=(const release_sync&) = delete;
private:
    release_machine* m_ = nullptr;
};

} // namespace detail
} // namespace jai

#endif
