#pragma once

#ifndef __JAISCRIPT_DETAIL_TYPE_NAME_KEY_HPP__
#define __JAISCRIPT_DETAIL_TYPE_NAME_KEY_HPP__

#include <string>
#include <string_view>

namespace jai {

class engine;

namespace detail {

    // The spelling a type name arriving as text is matched by: a_text without its whitespace (`Duo< int , string >` and
    // `Duo<int, string>` are one key). Never printed or saved.
    std::string type_name_key(std::string_view a_text);

    // The name a class is registered under for a name that arrived as text (reflect:: by name, an archive's "$type"): a_text
    // when it is registered as spelled, else the registered spelling of the same key (the engine's classes, then the
    // polymorphic registry's names), else empty. A miss walks every registered name: call it where the exact lookup failed.
    std::string registered_type_spelling(const engine* a_engine, std::string_view a_text);

}

}

#endif // __JAISCRIPT_DETAIL_TYPE_NAME_KEY_HPP__
