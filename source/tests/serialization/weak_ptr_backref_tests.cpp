// Reproduction test for weak_ptr back-reference resolution through a
// polymorphic shared_ptr load path. Mirrors a scene-graph host:
//   Node (property_owner, load_and_construct)
//     -> vector<shared_ptr<Component>>  (polymorphic, $type-tagged)
//          -> Component has weak_ptr<Node> owner  (back-reference to parent)
//
// The parent Node is loaded BEFORE the child component, so the weak_ptr is a
// pure backward reference. It must resolve to the owning Node after load.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/properties.hpp>
#include <jaiscript/serialization/archive.hpp>
#include <jaiscript/serialization/construct.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/serialization/polymorphic.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include "serialization_roundtrip.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class ReproNode;

// A large, default-constructible nested payload (mirrors PathMap's huge `map`).
// Default-constructible shared_ptr element -> exercises the non-load_and_construct
// shared_ptr branch with lots of nested begin_object/begin_array activity.
class ReproBlob : public jai::property_owner<ReproBlob> {
    friend jai::access;
public:
    ReproBlob() = default;
    void fill(int n) {
        std::vector<int> v;
        for (int i = 0; i < n; ++i) v.push_back(i);
        numbers = v;
        std::vector<std::string> s;
        for (int i = 0; i < n; ++i) s.push_back("item_" + std::to_string(i));
        labels = s;
    }
    size_t numberCount() const { return numbers.get().size(); }
private:
    JAI_PROPERTY((std::vector<int>), numbers);
    JAI_PROPERTY((std::vector<std::string>), labels);
};

class ReproComponent : public jai::property_owner<ReproComponent>,
                       public std::enable_shared_from_this<ReproComponent> {
    friend jai::access;
public:
    virtual ~ReproComponent() = default;

    std::shared_ptr<ReproNode> ownerLock() const { return owner.get().lock(); }
    void setOwner(const std::shared_ptr<ReproNode>& n) { owner = std::weak_ptr<ReproNode>(n); }
    std::string getId() const { return componentId.get(); }
    void setId(const std::string& v) { componentId = v; }
    void setBlob(const std::shared_ptr<ReproBlob>& b) { blob = b; }
    std::shared_ptr<ReproBlob> getBlob() const { return blob.get(); }

    // Mirrors the real Component(weak_ptr<Node>) constructor: a single arg so
    // construct(arg) binds cleanly under MSVC two-phase lookup.
    explicit ReproComponent(int /*dummy*/) {}

protected:
    template<typename Archive>
    void save(Archive& ar, std::uint32_t) const {
        property_mgr.save(ar);
    }
    template<typename Archive>
    void load(Archive& ar, std::uint32_t) {
        property_mgr.load(ar);
    }
    template<typename Archive>
    static void load_and_construct(Archive& ar, jai::serialization::construct<ReproComponent>& c) {
        c(0);
        c->load(ar, 0);
    }

private:
    // "blob" sorts before "componentId" and "owner", so this large nested
    // shared_ptr is deserialized BEFORE the weak_ptr owner — mirroring how
    // PathMap's huge `map` is read before `componentOwner`.
    JAI_PROPERTY((std::shared_ptr<ReproBlob>), blob);
    JAI_PROPERTY((std::string), componentId, "");
    JAI_PROPERTY((std::weak_ptr<ReproNode>), owner);
};

class ReproDerived : public jai::property_owner<ReproDerived, ReproComponent> {
    friend jai::access;
public:
    int getDerivedData() const { return derivedData.get(); }
    void setDerivedData(int v) { derivedData = v; }

    explicit ReproDerived(int dummy) : jai::property_owner<ReproDerived, ReproComponent>(dummy) {}

protected:
    template<typename Archive>
    void save(Archive& ar, std::uint32_t) const {
        property_mgr.save(ar);
        ReproComponent::save(ar, 0);
    }
    template<typename Archive>
    void load(Archive& ar, std::uint32_t) {
        property_mgr.load(ar);
        ReproComponent::load(ar, 0);
    }
    template<typename Archive>
    static void load_and_construct(Archive& ar, jai::serialization::construct<ReproDerived>& c) {
        c(0);
        c->load(ar, 0);
    }

private:
    JAI_PROPERTY((int), derivedData, 0);
};

class ReproNode : public jai::property_owner<ReproNode>,
                  public std::enable_shared_from_this<ReproNode> {
    friend jai::access;
public:
    explicit ReproNode(int /*dummy*/) {}

    std::string getNodeId() const { return nodeId.get(); }
    void setNodeId(const std::string& v) { nodeId = v; }
    std::vector<std::shared_ptr<ReproComponent>>& getComponents() { return components.get(); }

    template<typename Archive>
    void save(Archive& ar, std::uint32_t) const {
        property_mgr.save(ar);
    }
    template<typename Archive>
    static void load_and_construct(Archive& ar, jai::serialization::construct<ReproNode>& c) {
        c(0);
        c->property_mgr.load(ar);
    }

private:
    JAI_PROPERTY((std::string), nodeId, "");
    JAI_PROPERTY((std::vector<std::shared_ptr<ReproComponent>>), components);
};

class ReproTreeBase : public jai::property_owner<ReproTreeBase>,
                      public std::enable_shared_from_this<ReproTreeBase> {
    friend jai::access;
public:
    virtual ~ReproTreeBase() = default;
    explicit ReproTreeBase(int /*dummy*/) {}

    int getValue() const { return value.get(); }
    void setValue(int v) { value = v; }
    std::shared_ptr<ReproTreeBase> parentLock() const { return parent.get().lock(); }
    void setParent(const std::shared_ptr<ReproTreeBase>& p) { parent = std::weak_ptr<ReproTreeBase>(p); }
    std::vector<std::shared_ptr<ReproTreeBase>>& getChildren() { return children.get(); }

protected:
    ReproTreeBase() = default;
    template<typename Archive>
    void save(Archive& ar, std::uint32_t) const { property_mgr.save(ar); }
    template<typename Archive>
    void load(Archive& ar, std::uint32_t) { property_mgr.load(ar); }
    template<typename Archive>
    static void load_and_construct(Archive& ar, jai::serialization::construct<ReproTreeBase>& c) {
        c(0);
        c->load(ar, 0);
    }

private:
    JAI_PROPERTY((int), value, 0);
    JAI_PROPERTY((std::weak_ptr<ReproTreeBase>), parent);
    JAI_PROPERTY((std::vector<std::shared_ptr<ReproTreeBase>>), children);
};

class ReproTreeNode : public jai::property_owner<ReproTreeNode, ReproTreeBase> {
    friend jai::access;
public:
    explicit ReproTreeNode(int dummy) : jai::property_owner<ReproTreeNode, ReproTreeBase>(dummy) {}

    std::string getTag() const { return tag.get(); }
    void setTag(const std::string& v) { tag = v; }

protected:
    template<typename Archive>
    void save(Archive& ar, std::uint32_t) const { property_mgr.save(ar); ReproTreeBase::save(ar, 0); }
    template<typename Archive>
    void load(Archive& ar, std::uint32_t) { property_mgr.load(ar); ReproTreeBase::load(ar, 0); }
    template<typename Archive>
    static void load_and_construct(Archive& ar, jai::serialization::construct<ReproTreeNode>& c) {
        c(0);
        c->load(ar, 0);
    }

private:
    JAI_PROPERTY((std::string), tag, "");
};

struct ReadOrderItem {
    int weight = 0;

    template<typename Archive>
    void save(Archive& ar) const { ar.serialize("weight", weight); }
    template<typename Archive>
    void load(Archive& ar) { ar.serialize("weight", weight); }
};

struct ReadOrderSharedPair {
    std::shared_ptr<ReadOrderItem> inventory;
    std::shared_ptr<ReadOrderItem> equipped;

    template<typename Archive>
    void save(Archive& ar) const {
        ar.serialize("inventory", inventory);
        ar.serialize("equipped", equipped);
    }
    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("inventory", inventory);
        ar.serialize("equipped", equipped);
    }
};

struct ReadOrderSharedPairReversed {
    std::shared_ptr<ReadOrderItem> inventory;
    std::shared_ptr<ReadOrderItem> equipped;

    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("equipped", equipped);
        ar.serialize("inventory", inventory);
    }
};

struct ReadOrderSharedThenWeak {
    std::shared_ptr<ReadOrderItem> owner;
    std::weak_ptr<ReadOrderItem> watcher;

    template<typename Archive>
    void save(Archive& ar) const {
        ar.serialize("owner", owner);
        ar.serialize("watcher", watcher);
    }
    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("watcher", watcher);
        ar.serialize("owner", owner);
    }
};

struct ReadOrderWeakThenShared {
    std::shared_ptr<ReadOrderItem> owner;
    std::weak_ptr<ReadOrderItem> watcher;

    template<typename Archive>
    void save(Archive& ar) const {
        ar.serialize("watcher", watcher);
        ar.serialize("owner", owner);
    }
    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("owner", owner);
        ar.serialize("watcher", watcher);
    }
};

template<typename Out, typename In>
Out reload_json_as(jai::engine& eng, const In& in) {
    return jai::from_json<Out>(eng, jai::to_json(eng, in));
}

template<typename Out, typename In>
Out reload_binary_as(jai::engine& eng, const In& in) {
    return jai::from_binary_string<Out>(eng, jai::to_binary_string(eng, in));
}

class weak_ptr_backref_tests : public suite {
public:
    weak_ptr_backref_tests() : suite("WeakPtr Backref Tests") {}

    // Recursively verify every node's parent weak_ptr matches the actual tree
    // structure and points to the SAME instance held by the tree (not a copy).
    static void verifyTree(const std::shared_ptr<ReproTreeBase>& node,
                           const std::shared_ptr<ReproTreeBase>& expectedParent) {
        check(node != nullptr);
        if (!node) return;
        auto resolvedParent = node->parentLock();
        check_eq(resolvedParent.get(), expectedParent.get());
        for (auto& child : node->getChildren()) {
            verifyTree(child, node);
        }
    }

    void forge_tests() override {
        test("weak_ptr_backref_through_polymorphic_component", [&]() {
            auto eng = make_engine();

            serialization::polymorphic_registry::try_auto_register<ReproDerived>("ReproDerived");
            serialization::polymorphic_registry::try_auto_register<ReproComponent>("ReproComponent");

            auto run = [&](const std::string& fmt, auto&& roundtrip) {
                auto node = std::make_shared<ReproNode>(0);
                node->setNodeId("root");
                auto comp = std::make_shared<ReproDerived>(0);
                comp->setId("comp_1");
                comp->setDerivedData(99);
                auto blob = std::make_shared<ReproBlob>();
                blob->fill(500);  // large nested payload read before `owner`
                comp->setBlob(blob);
                comp->setOwner(node);  // backward reference
                node->getComponents().push_back(comp);

                std::shared_ptr<ReproNode> loaded = roundtrip(*eng, node);

                check(loaded != nullptr, fmt + ": node loaded");
                if (!loaded) { return; }
                check_eq(loaded->getNodeId(), std::string("root"));
                check_eq(loaded->getComponents().size(), size_t(1));
                if (loaded->getComponents().empty()) { return; }

                auto loadedComp = loaded->getComponents()[0];
                check(loadedComp != nullptr, fmt + ": component loaded");
                if (!loadedComp) { return; }
                check_eq(loadedComp->getId(), std::string("comp_1"));

                check(loadedComp->getBlob() != nullptr, fmt + ": blob loaded");
                if (loadedComp->getBlob()) { check_eq(loadedComp->getBlob()->numberCount(), size_t(500)); }

                // The crux: the component's weak_ptr owner must resolve to the node.
                auto resolvedOwner = loadedComp->ownerLock();
                check(resolvedOwner != nullptr, fmt + ": weak owner resolved");
                check_eq(resolvedOwner.get(), loaded.get());
            };

            run("json", [](engine& e, const auto& v) { return roundtrip_json(e, v); });
            run("binary", [](engine& e, const auto& v) { return roundtrip_binary(e, v); });
        });

        test("polymorphic_tree_parent_backrefs", [&]() {
            auto eng = make_engine();
            serialization::polymorphic_registry::try_auto_register<ReproTreeNode>("ReproTreeNode");

            auto run = [&](const std::string& fmt, auto&& roundtrip) {
                auto root = std::make_shared<ReproTreeNode>(0);
                root->setValue(1); root->setTag("root");

                auto childA = std::make_shared<ReproTreeNode>(0);
                childA->setValue(2); childA->setTag("A"); childA->setParent(root);
                root->getChildren().push_back(childA);

                auto childB = std::make_shared<ReproTreeNode>(0);
                childB->setValue(3); childB->setTag("B"); childB->setParent(root);
                root->getChildren().push_back(childB);

                auto grandA = std::make_shared<ReproTreeNode>(0);
                grandA->setValue(4); grandA->setTag("A1"); grandA->setParent(childA);
                childA->getChildren().push_back(grandA);

                // Serialize through the BASE static type so every node is $type-tagged.
                std::shared_ptr<ReproTreeBase> rootBase = root;
                std::shared_ptr<ReproTreeBase> loaded = roundtrip(*eng, rootBase);

                check(loaded != nullptr, fmt + ": tree loaded");
                if (!loaded) { return; }
                check_eq(loaded->parentLock().get(), (ReproTreeBase*)nullptr);  // root has no parent
                check_eq(loaded->getChildren().size(), size_t(2));

                verifyTree(loaded, nullptr);
            };

            run("json", [](engine& e, const auto& v) { return roundtrip_json(e, v); });
            run("binary", [](engine& e, const auto& v) { return roundtrip_binary(e, v); });
        });
        test("a_back_reference_read_before_its_definition", [&]() {
            auto eng = make_engine();
            auto danglingText = [](auto&& load) {
                try {
                    load();
                } catch (const serialization_error& e) {
                    return std::string(e.what());
                }
                return std::string();
            };
            auto run = [&](const std::string& fmt, auto&& reloadShared, auto&& reloadReversed, auto&& reloadWeakFirstRead, auto&& reloadSharedFirstRead) {
                auto item = std::make_shared<ReadOrderItem>();
                item->weight = 7;

                ReadOrderSharedPair pair;
                pair.inventory = item;
                pair.equipped = item;
                auto inOrder = reloadShared(pair);
                check(inOrder.inventory != nullptr && inOrder.inventory == inOrder.equipped, fmt + ": read in the written order, both fields share one object");

                auto reversedError = danglingText([&]() { reloadReversed(pair); });
                check(reversedError.find("Dangling shared_ptr reference") != std::string::npos, fmt + ": a $id back-reference read before its definition throws, got: " + reversedError);

                ReadOrderSharedThenWeak sharedThenWeak;
                sharedThenWeak.owner = item;
                sharedThenWeak.watcher = item;
                auto weakFirst = reloadWeakFirstRead(sharedThenWeak);
                check(weakFirst.owner != nullptr && weakFirst.owner->weight == 7, fmt + ": the defining shared_ptr loads");
                check(weakFirst.watcher.expired(), fmt + ": a $ref back-reference read before its definition loads empty");

                ReadOrderWeakThenShared weakThenShared;
                weakThenShared.owner = item;
                weakThenShared.watcher = item;
                auto weakDefinedError = danglingText([&]() { reloadSharedFirstRead(weakThenShared); });
                check(weakDefinedError.find("Dangling shared_ptr reference") != std::string::npos, fmt + ": a $id read before the weak_ptr that defines the object throws, got: " + weakDefinedError);
            };

            run("json",
                [&](const auto& v) { return reload_json_as<ReadOrderSharedPair>(*eng, v); },
                [&](const auto& v) { return reload_json_as<ReadOrderSharedPairReversed>(*eng, v); },
                [&](const auto& v) { return reload_json_as<ReadOrderSharedThenWeak>(*eng, v); },
                [&](const auto& v) { return reload_json_as<ReadOrderWeakThenShared>(*eng, v); });
            run("binary",
                [&](const auto& v) { return reload_binary_as<ReadOrderSharedPair>(*eng, v); },
                [&](const auto& v) { return reload_binary_as<ReadOrderSharedPairReversed>(*eng, v); },
                [&](const auto& v) { return reload_binary_as<ReadOrderSharedThenWeak>(*eng, v); },
                [&](const auto& v) { return reload_binary_as<ReadOrderWeakThenShared>(*eng, v); });
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::weak_ptr_backref_tests)
