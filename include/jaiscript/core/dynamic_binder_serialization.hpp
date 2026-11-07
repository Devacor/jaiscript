#pragma once

// Context-based deserialization factories: make_context_only_factory hands the factory the archive's user context,
// make_context_archive_factory the context and the archive

#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/serialization/archive_impl.hpp>

namespace jai {
namespace dynamic_binder_detail {

    // Type-erased context extractor implementation
    using context_extractor_t = std::function<void*(serialization::any_archive_reader&, const std::string&)>;

    // Helper to create context extractor for a specific type
    template<typename ContextType>
    context_extractor_t make_context_extractor() {
        return [](serialization::any_archive_reader& archive, const std::string& type_name) -> void* {
            auto ctx = archive.template get_user_context<ContextType>();
            if (!ctx) {
                throw serialization_error("User context of type '" + type_name +
                                         "' not found in archive");
            }
            return static_cast<void*>(ctx);
        };
    }

    // Factory implementation that uses type-erased context extraction (context-only version)
    template<typename T, typename ContextType, typename FactoryFunc>
    std::function<script_value(serialization::any_archive_reader&, uint32_t)>
    make_context_only_factory(FactoryFunc&& factory, std::string class_name, engine* engine_ptr) {
        auto extractor = make_context_extractor<ContextType>();
        std::string context_type_name = typeid(ContextType).name();

        return [factory = std::forward<FactoryFunc>(factory),
                class_name = std::move(class_name),
                engine_ptr,
                extractor = std::move(extractor),
                context_type_name = std::move(context_type_name)]
               (serialization::any_archive_reader& archive, uint32_t) -> script_value {

            void* raw_context = extractor(archive, context_type_name);
            auto* user_context = static_cast<ContextType*>(raw_context);

            auto cpp_obj = factory(user_context);
            return wrap_cpp_object<T>(cpp_obj, class_name, engine_ptr);
        };
    }

    // Factory implementation that uses type-erased context extraction (context + archive version)
    template<typename T, typename ContextType, typename FactoryFunc>
    std::function<script_value(serialization::any_archive_reader&, uint32_t)>
    make_context_archive_factory(FactoryFunc&& factory, std::string class_name, engine* engine_ptr) {
        auto extractor = make_context_extractor<ContextType>();
        std::string context_type_name = typeid(ContextType).name();

        return [factory = std::forward<FactoryFunc>(factory),
                class_name = std::move(class_name),
                engine_ptr,
                extractor = std::move(extractor),
                context_type_name = std::move(context_type_name)]
               (serialization::any_archive_reader& archive, uint32_t) -> script_value {

            void* raw_context = extractor(archive, context_type_name);
            auto* user_context = static_cast<ContextType*>(raw_context);

            auto cpp_obj = factory(user_context, archive);
            return wrap_cpp_object<T>(cpp_obj, class_name, engine_ptr);
        };
    }

} // namespace dynamic_binder_detail
} // namespace jai
