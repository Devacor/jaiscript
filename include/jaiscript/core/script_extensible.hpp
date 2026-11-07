// Part of the engine's closure, which includes it in its order: a translation unit that names it first is sent there
#if !defined(__JAISCRIPT_CORE_ENGINE_HPP__)
#include "engine.hpp"
#elif !defined(__JAISCRIPT_CORE_SCRIPT_EXTENSIBLE_HPP__)
#define __JAISCRIPT_CORE_SCRIPT_EXTENSIBLE_HPP__

#include <memory>
#include <string>

namespace jai {

class class_instance;

namespace detail {
    // Out of line (engine.cpp): marks a surviving layer retired when its object dies.
    void retire_script_layer(const weaker_ptr<class_instance>& layer);
    std::string script_layer_to_json(const strong_ptr<class_instance>& layer);
    bool script_layer_from_json(const strong_ptr<class_instance>& layer, const std::string& json);
}

// Lets a C++ object carry the script class instance extending it (docs/site/embedding/13-script-extensible.html).
// Two ownership directions share the slot: a script-constructed hybrid's layer owns its
// object (the slot only points back, weakly); an EXTENDED object owns its layer (strong), and
// the layer references the object without owning it. Either way handles -> layer -> object
// never cycles. When an object dies while a script still holds its layer, the layer is
// retired: C++ access through it reports instead of dangling. property_owner derives this
// for you; derive it directly on a type with nothing to reflect. A copied or moved object is
// a new identity and starts without a layer.
class script_extensible {
public:
    strong_ptr<class_instance> script_layer() const { return layer_.lock(); }
    bool has_script_layer() const { return !layer_.expired(); }
    class_instance* peek_script_layer() const noexcept { return layer_.peek(); }   // no count: a parallel region's reads

    // The layer's script state as {"$type": class, "$script": fields} - what extend's
    // fields_json and a cross-engine move speak. "" without a layer. from_json hydrates the
    // live layer by field name, re-deriving first when $type names another class; false
    // without a layer (create one with engine::extend(object, "$type", json) instead).
    std::string script_layer_to_json() const { return detail::script_layer_to_json(layer_.lock()); }
    bool script_layer_from_json(const std::string& json) { return detail::script_layer_from_json(layer_.lock(), json); }

    // The slot itself. property_owner re-exports this under multiple inheritance so the
    // binder always reaches one unambiguous slot.
    script_extensible& jai_script_slot() { return *this; }
    const script_extensible& jai_script_slot() const { return *this; }

    // constructed hybrid: the layer owns the object, the slot points back
    void attach_script_layer(const weaker_ptr<class_instance>& layer) { layer_ = layer; }
    // extended object: the object owns its layer
    void own_script_layer(const strong_ptr<class_instance>& layer) { owned_ = layer; layer_ = layer; }
    void detach_script_layer() { owned_.reset(); layer_.reset(); }

protected:
    script_extensible() = default;
    ~script_extensible() { if (!layer_.expired()) { detail::retire_script_layer(layer_); } }
    script_extensible(const script_extensible&) noexcept {}
    script_extensible(script_extensible&&) noexcept {}
    script_extensible& operator=(const script_extensible&) noexcept { return *this; }
    script_extensible& operator=(script_extensible&&) noexcept { return *this; }

private:
    strong_ptr<class_instance> owned_;
    weaker_ptr<class_instance> layer_;
};

namespace detail {
    template<typename... Bases>
    inline constexpr bool any_script_extensible_v = (std::is_base_of_v<script_extensible, Bases> || ...);

    template<typename... Bases> struct first_script_extensible { using type = void; };
    template<typename B, typename... Rest>
    struct first_script_extensible<B, Rest...> {
        using type = std::conditional_t<std::is_base_of_v<script_extensible, B>, B,
                                        typename first_script_extensible<Rest...>::type>;
    };
    template<typename... Bases>
    using first_script_extensible_t = typename first_script_extensible<Bases...>::type;

    // Per-owner tag so a multi-level chain never carries the same empty base twice
    template<typename Owner> struct no_script_slot {};
}

}

#endif // __JAISCRIPT_CORE_SCRIPT_EXTENSIBLE_HPP__
