# JaiScript Properties System

A generic, reflection-enabled property system with built-in serialization support.

## Features

- **Transparent value access** - Properties behave like their underlying type
- **Automatic reflection** - All properties auto-register with `property_manager`
- **Serialization support** - JSON and binary archives built-in
- **Version migration** - Add/remove/rename fields with backward compatibility
- **Selective serialization** - Enable/disable per-property
- **Clone support** - Deep copy with custom clone functions
- **Container support** - Works with `std::vector`, `std::map`, etc.

## Quick Start

```cpp
#include <jaiscript/properties/property.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/properties/property_serialization.hpp>

// CRTP pattern: property_owner<DerivedClass, OptionalBases...>
class Player : public jai::property_owner<Player> {
public:
    // Note: Types MUST be wrapped in parentheses
    JAI_PROPERTY((int), health, 100);
    JAI_PROPERTY((float), speed, 5.0f);
    JAI_PROPERTY((std::string), name, "Player");
    JAI_PROPERTY((bool), active, true);

    // Complex types need parentheses too
    JAI_PROPERTY((std::vector<int>), scores);
    JAI_PROPERTY((std::map<std::string, int>), inventory);
};

// Usage
Player p;
p.health = 50;                    // Direct assignment
int h = p.health;                 // Implicit conversion
p.health += 10;                   // Arithmetic operators
if (p.health > 0) { /*...*/ }    // Comparison operators

// Reflection
for (auto& [name, prop] : p.property_mgr.all()) {
    std::cout << name << std::endl;
}

// Serialization
#include <jaiscript/serialization/json_archive.hpp>

jai::serialization::json_archive_writer writer;
writer.begin_object("Player", 1);
p.property_mgr.save(writer);
writer.end_object();

std::string json = writer.str();
```

## Macros

### JAI_PROPERTY(type, name, default_value...)
Declares a property with optional default value.

**Important**: Types **must** be wrapped in parentheses to handle commas in template arguments.

```cpp
JAI_PROPERTY((int), health, 100);
JAI_PROPERTY((std::string), name, "Default");
JAI_PROPERTY((std::vector<int>), numbers);  // No default
JAI_PROPERTY((std::map<std::string, float>), data);
```

### JAI_DELETED_PROPERTY(type, name)
Marks a property as deleted for version migration. Allows old serialized data to be skipped.

```cpp
JAI_DELETED_PROPERTY((int), old_field);
```

### JAI_NAMED_PROPERTY(type, prop_name, var_name, default_value...)
Creates a property with different property name vs variable name.

```cpp
JAI_NAMED_PROPERTY((int), "healthPoints", health, 100);
// Property name in serialization: "healthPoints"
// Variable name in code: health
```

## Property Features

### Transparent Access
Properties support all operators of their underlying type:

```cpp
JAI_PROPERTY((int), count, 0);

count = 10;           // Assignment
count += 5;           // Compound assignment
++count;              // Increment
int x = count * 2;    // Arithmetic
if (count > 5) {}     // Comparison
```

### Container Support
Properties work seamlessly with STL containers:

```cpp
JAI_PROPERTY((std::vector<int>), numbers);

numbers.get().push_back(1);
for (int n : numbers) { /*...*/ }  // Range-based for
numbers[0] = 10;                   // Subscript operator
```

### Pointer Support
Properties handle smart pointers and raw pointers:

```cpp
JAI_PROPERTY((std::shared_ptr<Node>), node);

node->method();        // Arrow operator
if (node) { /*...*/ }  // Boolean conversion
```

### Serialization Control

```cpp
JAI_PROPERTY((int), secret_value, 42);

// Disable serialization for specific properties
secret_value.serialize_enabled(false);

// Check status
bool enabled = secret_value.serialize_enabled();
```

## Serialization

### Basic Serialization

```cpp
#include <jaiscript/serialization/json_archive.hpp>

// Write
jai::serialization::json_archive_writer writer;
writer.begin_object("MyClass", 1);  // version = 1
obj.property_mgr.save(writer);
writer.end_object();

std::string json = writer.str();

// Read
jai::serialization::json_archive_reader reader(json);
std::string type_name;
uint32_t version;
reader.begin_object(type_name, version);
obj.property_mgr.load(reader);
reader.end_object();
```

### Binary Serialization

```cpp
#include <jaiscript/serialization/binary_archive.hpp>

std::vector<uint8_t> buffer;
jai::serialization::binary_archive_writer writer(buffer);
writer.begin_object("MyClass", 1);
obj.property_mgr.save(writer);
writer.end_object();

// Read
jai::serialization::binary_archive_reader reader(buffer);
// ... same as JSON
```

## Polymorphic serialization — it just works

Every `property_owner` type registers itself with `jai::serialization::polymorphic_registry`
at program start, so `shared_ptr<Base>` round-trips get a `$type` tag and reconstruct the
correct dynamic type with **zero extra code**. Rules:

- The `$type` name is the bare class identifier (`"Player"` for `class Player`), parsed from
  the compiler signature — never a mangled `typeid` name, identical on MSVC/GCC/Clang. All
  namespace qualification is stripped, matching the bare names explicit registrars use, so a
  type can move between implicit and explicit registration without its `$type` changing.
  Anonymous-namespace types strip to their bare name too; they are per-TU distinct types, so
  two TUs deriving the same name trip the collision warning (pin one with `jai_type_name`).
- Pin a custom name in-class when the identifier might change or for template
  instantiations (which have no stable derived spelling and are otherwise skipped):

  ```cpp
  static constexpr const char* jai_type_name = "MyStableName";
  ```

- To name a type explicitly **without modifying it**, register externally at file scope —
  either `jai::registrar<T, Ctx>("Name")` (also adds script bindings) or
  `jai::serialization::polymorphic_registry::try_auto_register<T>("Name")` (serialization
  only). Both take precedence over the implicit name regardless of static-init order. Every
  path feeds the same registry — there is exactly one source of serialization identity.
- Saving an *unregistered* polymorphic type through a base pointer throws
  `serialization_error` instead of silently slicing.
- Implicit registration is keyed to construction code: a binary that only ever *loads* a
  type and never constructs it anywhere should keep an explicit `jai::registrar`.
- Name collisions (two types deriving the same name) are reported on stderr and the later
  implicit registration is skipped — disambiguate with `jai_type_name` or a registrar.

## Inheritance

The `property_owner` template uses CRTP (Curiously Recurring Template Pattern) and supports inheritance tracking for JaiScript binding:

### Single Inheritance

```cpp
// Base class
class Entity : public jai::property_owner<Entity> {
public:
    JAI_PROPERTY((int), id, 0);
    JAI_PROPERTY((std::string), name);
};

// Derived class - specify base in template args
class Player : public jai::property_owner<Player, Entity> {
public:
    JAI_PROPERTY((int), health, 100);
    JAI_PROPERTY((int), level, 1);
};

// Further derived
class Warrior : public jai::property_owner<Warrior, Player> {
public:
    JAI_PROPERTY((int), strength, 10);
};
```

### Multiple Inheritance

```cpp
// Multiple base classes
class Named : public jai::property_owner<Named> {
public:
    JAI_PROPERTY((std::string), name);
};

class Positioned : public jai::property_owner<Positioned> {
public:
    JAI_PROPERTY((float), x, 0.0f);
    JAI_PROPERTY((float), y, 0.0f);
};

// Derive from multiple bases (no diamond inheritance allowed)
class Entity : public jai::property_owner<Entity, Named, Positioned> {
public:
    JAI_PROPERTY((int), id, 0);
};
```

The inheritance information is exposed via `_jai_base_types` and used by `dynamic_binder::auto_bind()` to automatically register base classes in JaiScript.

## Version Migration

### Adding New Fields

```cpp
// Version 1
class Player_v1 : public jai::property_owner<Player_v1> {
    JAI_PROPERTY((int), x);
    JAI_PROPERTY((int), y);
};

// Version 2 (added z)
class Player_v2 : public jai::property_owner<Player_v2> {
    JAI_PROPERTY((int), x);
    JAI_PROPERTY((int), y);
    JAI_PROPERTY((int), z, 0);  // New field with default
};

// Load v1 data into v2: nothing extra to do. Loads seek properties BY NAME (JSON) or by
// stored key (binary), so added fields simply keep their defaults.
player_v2.property_mgr.load(reader);
```

### Renaming Fields

```cpp
// Version 2 (field named "label")
class Object_v2 : public jai::property_owner<Object_v2> {
    JAI_PROPERTY((std::string), label);
};

// Version 3 (renamed to "description")
class Object_v3 : public jai::property_owner<Object_v3> {
    JAI_PROPERTY((std::string), description);
    JAI_DELETED_PROPERTY((std::string), label);   // consumes (discards) the old value
};

// Load v2 data into v3
obj_v3.property_mgr.load(reader);
```

Note: there is no automatic value migration on rename — the deleted placeholder DISCARDS
the old value (pinned by the `version_rename_field` test) and the renamed field keeps its
default. To carry the value across a rename, migrate it in `post_load` by reading the old
name explicitly before adopting this pattern.

### Removing Fields

```cpp
// Version 3
class Player_v3 : public jai::property_owner<Player_v3> {
    JAI_PROPERTY((int), x);
    JAI_PROPERTY((int), y);
    JAI_DELETED_PROPERTY((int), old_field);  // Removed field
};

// Old serialized data will skip "old_field"
```

## Clone Support

```cpp
// Custom deep copy
JAI_PROPERTY((std::shared_ptr<Node>), node, nullptr,
    [](auto& src, auto& dst) {
        if (src.get()) {
            dst = std::make_shared<Node>(*src.get());
        }
    });

// Clone to another object
source_obj.property_mgr.clone_to_target(dest_obj.property_mgr);
```

## Architecture

- **property_base**: Abstract base class for type-erased property access
- **property<T>**: Template class implementing typed property
- **deleted_property<T>**: Placeholder for removed fields (versioning)
- **property_manager**: Reflection registry, owns property references
- **property_owner**: Base class providing `property_mgr` member

## Declaring properties

```cpp
// CRTP: pass the derived type as the first template argument
class MyClass : public jai::property_owner<MyClass> {
    JAI_PROPERTY((int), health, 100);
    JAI_PROPERTY((std::string), name);
};

// With inheritance
class Derived : public jai::property_owner<Derived, MyClass> {
    JAI_PROPERTY((float), speed, 5.0f);
};
```

- `property_owner<T>` uses CRTP (the derived type is the first template argument)
- Bases follow it as template arguments; they drive auto-binding and serialization
- Properties serialize through the JaiScript archives

## Naming Conventions

All JaiScript code uses **snake_case**:
- Classes: `property_manager`, `property_owner`, `property_base`
- Methods: `serialize_enabled()`, `allow_save()`, `clone_to_target()`
- Members: `property_mgr`, `m_value`, `m_allow_serialization`

