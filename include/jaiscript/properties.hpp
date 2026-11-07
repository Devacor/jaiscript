#pragma once
#ifndef __JAI_PROPERTIES_HPP__
#define __JAI_PROPERTIES_HPP__
// Convenience header for JaiScript properties
// Usage: #include <jaiscript/properties.hpp>

#include <jaiscript/properties/property.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/macros.hpp>
// Include property_serialization.hpp explicitly in .cpp files that serialize properties.

// For custom types (like game::Point<float>), provide
// save() and load() free functions in the same namespace as your type (ADL):
//
//   namespace game {
//     void save(jai::serialization::archive_writer& ar, const Point<float>& p) {
//       ar.write_float32(p.x);
//       ar.write_float32(p.y);
//       ar.write_float32(p.z);
//     }
//     void load(jai::serialization::archive_reader& ar, Point<float>& p) {
//       p.x = ar.read_float32();
//       p.y = ar.read_float32();
//       p.z = ar.read_float32();
//     }
//   }

#endif
