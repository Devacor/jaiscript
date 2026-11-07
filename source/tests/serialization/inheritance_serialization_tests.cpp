#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/properties/property.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include "serialization_roundtrip.hpp"

#include <memory>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// property_owner<Derived, Base> inheritance: pins that the whole chain round-trips on every archive
// format and via the polymorphic path.

class InhBase : public property_owner<InhBase> {
public:
    JAI_PROPERTY((int), baseVal, 1);
};

class InhDerived : public property_owner<InhDerived, InhBase> {
public:
    JAI_PROPERTY((std::string), derivedTag, "");
};

// Three levels, so the fix must recurse, not just peek one base up.
class InhMid : public property_owner<InhMid, InhBase> {
public:
    JAI_PROPERTY((int), midVal, 2);
};
class InhLeaf : public property_owner<InhLeaf, InhMid> {
public:
    JAI_PROPERTY((std::string), leafTag, "");
};

class inheritance_serialization_tests : public suite {
public:
    inheritance_serialization_tests() : suite("Inheritance Serialization") {}

    void forge_tests() override {

        test("inherited_property_survives_json", [&]() {
            auto eng = make_engine();
            auto derived = std::make_shared<InhDerived>();
            derived->baseVal = 42;
            derived->derivedTag = "kept";
            std::shared_ptr<InhBase> original = derived;

            auto loaded = roundtrip_json(*eng, original);
            auto ld = std::dynamic_pointer_cast<InhDerived>(loaded);
            check(ld != nullptr, "dynamic type preserved");
            check_eq(std::string("kept"), ld->derivedTag.get());
            check_eq(42, ld->baseVal.get());
        });

        test("inherited_property_survives_binary", [&]() {
            auto eng = make_engine();
            auto derived = std::make_shared<InhDerived>();
            derived->baseVal = 99;
            derived->derivedTag = "bin";
            std::shared_ptr<InhBase> original = derived;

            auto loaded = roundtrip_binary(*eng, original);
            auto ld = std::dynamic_pointer_cast<InhDerived>(loaded);
            check(ld != nullptr, "dynamic type preserved (binary)");
            check_eq(std::string("bin"), ld->derivedTag.get());
            check_eq(99, ld->baseVal.get());
        });

        test("deep_inheritance_chain_json", [&]() {
            auto eng = make_engine();
            auto leaf = std::make_shared<InhLeaf>();
            leaf->baseVal = 10;
            leaf->midVal = 20;
            leaf->leafTag = "deep";
            std::shared_ptr<InhBase> original = leaf;

            auto loaded = roundtrip_json(*eng, original);
            auto ll = std::dynamic_pointer_cast<InhLeaf>(loaded);
            check(ll != nullptr, "deep dynamic type preserved");
            check_eq(10, ll->baseVal.get());
            check_eq(20, ll->midVal.get());
            check_eq(std::string("deep"), ll->leafTag.get());
        });

        test("deep_inheritance_chain_binary", [&]() {
            auto eng = make_engine();
            auto leaf = std::make_shared<InhLeaf>();
            leaf->baseVal = 11;
            leaf->midVal = 22;
            leaf->leafTag = "deepb";
            std::shared_ptr<InhBase> original = leaf;

            auto loaded = roundtrip_binary(*eng, original);
            auto ll = std::dynamic_pointer_cast<InhLeaf>(loaded);
            check(ll != nullptr, "deep dynamic type preserved (binary)");
            check_eq(11, ll->baseVal.get());
            check_eq(22, ll->midVal.get());
            check_eq(std::string("deepb"), ll->leafTag.get());
        });
    }
};

} // namespace jai::foundry::tests

using inheritance_serialization_tests = jai::foundry::tests::inheritance_serialization_tests;
FOUNDRY_REGISTER(inheritance_serialization_tests)
