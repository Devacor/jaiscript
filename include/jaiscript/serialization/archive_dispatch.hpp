#pragma once

#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <stdexcept>

namespace jai {
namespace serialization {

template<typename Fn>
decltype(auto) any_archive_writer::dispatch(Fn&& fn) {
    switch (id_) {
        case writer_archive_id::json:
            return fn(*static_cast<json_archive_writer*>(ptr_));
        case writer_archive_id::binary:
            return fn(*static_cast<binary_archive_writer*>(ptr_));
        default:
            throw std::runtime_error("Unknown writer archive type for dispatch");
    }
}

template<typename Fn>
decltype(auto) any_archive_reader::dispatch(Fn&& fn) {
    switch (id_) {
        case reader_archive_id::json:
            return fn(*static_cast<json_archive_reader*>(ptr_));
        case reader_archive_id::binary:
            return fn(*static_cast<binary_archive_reader*>(ptr_));
        default:
            throw std::runtime_error("Unknown reader archive type for dispatch");
    }
}

} // namespace serialization
} // namespace jai
