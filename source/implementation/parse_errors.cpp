#include <jaiscript/core/parse_errors.hpp>

namespace jai {

    std::string parse_error_category_impl::message(int ev) const {
        return static_cast<parse_error_code>(ev) == parse_error_code::success ? "Success" : "Syntax error";
    }

    const std::error_category& parse_error_category() noexcept {
        static parse_error_category_impl instance;
        return instance;
    }

} // namespace jai
