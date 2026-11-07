#include <jaiscript/serialization/polymorphic.hpp>

namespace jai::serialization {

polymorphic_registry& polymorphic_registry::instance() {
    static polymorphic_registry reg;
    return reg;
}

void polymorphic_registry::register_entry(std::type_index ti, type_entry entry, bool a_implicit) {
    std::unique_lock guard(mutex_);
    entry.type = ti;
    entry.implicit = a_implicit;
    auto existing = by_rtti_.find(ti);
    if (existing != by_rtti_.end() && a_implicit) {
        return;
    }
    if (auto named = by_name_.find(entry.name); named != by_name_.end() && named->second->type != ti) {
        if (a_implicit) {
            std::fprintf(stderr, "[jai::serialization] implicit registration of '%s' skipped: the name already belongs to a different type. Register one of them with an explicit jai::registrar name.\n", entry.name.c_str());
            return;
        }
        // An explicit registration owns its name outright; the displaced type would
        // otherwise resolve to the wrong factory on load, so evict it entirely (its
        // next save fails loudly instead of corrupting data).
        std::fprintf(stderr, "[jai::serialization] explicit registration of '%s' evicts a previous%s registration of the same name for a different type.\n", entry.name.c_str(), named->second->implicit ? " implicit" : " explicit");
        auto victimType = named->second->type;
        by_name_.erase(named);
        by_rtti_.erase(victimType);
        existing = by_rtti_.find(ti);
    }
    if (existing != by_rtti_.end() && existing->second.name != entry.name) {
        by_name_.erase(existing->second.name);
    }
    auto [it, _] = by_rtti_.insert_or_assign(ti, std::move(entry));
    by_name_[it->second.name] = &it->second;
}

auto polymorphic_registry::find(std::type_index ti) const -> const type_entry* {
    std::shared_lock guard(mutex_);
    auto it = by_rtti_.find(ti);
    return it != by_rtti_.end() ? &it->second : nullptr;
}

auto polymorphic_registry::find(const std::string& name) const -> const type_entry* {
    std::shared_lock guard(mutex_);
    auto it = by_name_.find(name);
    return it != by_name_.end() ? it->second : nullptr;
}

void polymorphic_registry::add_base_edge(std::type_index derived, std::type_index base, void* (*adjust)(void*)) {
    std::unique_lock guard(mutex_);
    auto& edges = base_edges_[derived];
    for (const auto& e : edges) { if (e.base == base) { return; } }
    edges.push_back(base_edge{ base, adjust });
}

void* polymorphic_registry::adjust_to_base(std::type_index from, std::type_index to, void* p) const {
    if (p == nullptr || from == to) { return p; }
    std::shared_lock guard(mutex_);
    auto it = base_edges_.find(from);
    if (it == base_edges_.end()) { return p; }
    for (const auto& e : it->second) { if (e.base == to) { return e.adjust(p); } }
    std::unordered_set<std::type_index> visited{ from };
    std::vector<std::pair<std::type_index, void*>> stack;
    for (const auto& e : it->second) {
        if (visited.insert(e.base).second) { stack.push_back({ e.base, e.adjust(p) }); }
    }
    while (!stack.empty()) {
        auto [cur, curp] = stack.back();
        stack.pop_back();
        auto cit = base_edges_.find(cur);
        if (cit == base_edges_.end()) { continue; }
        for (const auto& e : cit->second) {
            void* next = e.adjust(curp);
            if (e.base == to) { return next; }
            if (visited.insert(e.base).second) { stack.push_back({ e.base, next }); }
        }
    }
    return p;
}

bool polymorphic_registry::is_assignable(std::type_index from, std::type_index to) const {
    if (from == to || to == std::type_index(typeid(void))) { return true; }
    std::shared_lock guard(mutex_);
    auto it = base_edges_.find(from);
    if (it == base_edges_.end()) { return false; }
    std::unordered_set<std::type_index> visited{ from };
    std::vector<std::type_index> stack;
    for (const auto& e : it->second) {
        if (e.base == to) { return true; }
        if (visited.insert(e.base).second) { stack.push_back(e.base); }
    }
    while (!stack.empty()) {
        auto cur = stack.back();
        stack.pop_back();
        auto cit = base_edges_.find(cur);
        if (cit == base_edges_.end()) { continue; }
        for (const auto& e : cit->second) {
            if (e.base == to) { return true; }
            if (visited.insert(e.base).second) { stack.push_back(e.base); }
        }
    }
    return false;
}

} // namespace jai::serialization
