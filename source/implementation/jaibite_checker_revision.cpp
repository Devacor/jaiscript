// The one file that reads the generated revision: a new value recompiles it alone (declared in detail/ast_serializer.hpp)
#include <jaiscript/detail/jaibite_checker_revision.hpp>

namespace jai::detail {

    static_assert(k_jaibite_checker_revision <= 0xFFFFFFu, "a stamp holds the revision in 24 bits");

    uint32_t jaibite_checker_revision() noexcept { return k_jaibite_checker_revision; }

} // namespace jai::detail
