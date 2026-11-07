#include <jaiscript/stdlib/stdlib.hpp>

namespace jai {
namespace stdlib {

    void register_all(engine& eng_ref) {
        // Core functions
        register_json_functions(eng_ref);
        register_io_functions(eng_ref);
        register_container_types(eng_ref);
        register_string_functions(eng_ref);
        register_reflection_functions(eng_ref);   // the reflect:: capability — separately
                                                  // composable for hosts that grant stdlib
                                                  // without reflection
        register_time_functions(eng_ref);         // time:: over the engine's time source
        register_random_functions(eng_ref);       // the deterministic random type
        register_crypto_functions(eng_ref);       // crypto::

    }

} // namespace stdlib
} // namespace jai
