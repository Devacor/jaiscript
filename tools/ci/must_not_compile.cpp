// Programs the headers reject. tools/ci/gates.sh headers compiles this file once per `must_not_compile N: text`
// line below with -DJAI_MUST_NOT_COMPILE=N and wants the compile to fail with that text, and once with no case, which
// builds: the uses under `#else` are the same types through the routes whose error waits for run time.
//
// must_not_compile 1: declares serialize() but jai::access cannot reach it
// must_not_compile 2: declares save() but jai::access cannot reach it
// must_not_compile 3: declares load() but jai::access cannot reach it
// must_not_compile 4: declares load_and_construct() but jai::access cannot reach it
// must_not_compile 5: the archive cannot write this type, it has no serialization route
// must_not_compile 6: the archive cannot read this type, it has no serialization route
// must_not_compile 7: the archive cannot write this type, it has no serialization route
// must_not_compile 8: declares serialize() but jai::access cannot reach it
// must_not_compile 9: declares load_and_construct() but jai::access cannot reach it
// must_not_compile 10: the archive cannot construct this type on a load
// must_not_compile 11: declares load_and_construct() but jai::access cannot reach it
// must_not_compile 12: declares save() but jai::access cannot reach it
// must_not_compile 13: declares load() but jai::access cannot reach it
// must_not_compile 14: declares serialize() but jai::access cannot reach it
// must_not_compile 15: a jai::context<C> may only narrow its bases' context
// must_not_compile 16: a jai::context<C> may only narrow its bases' context
// must_not_compile 17: its bases carry different contexts
// must_not_compile 18: C must be the class's context
// must_not_compile 19: use jai::const_overload
// must_not_compile 20: T has no portable auto-name
// must_not_compile 21: declares exactly one jai_auto_bind, and not a template
// must_not_compile 22: a class with no context declares static void jai_auto_bind(jai::dynamic_binder<T>&), without a context
// must_not_compile 23: a class with a context C declares static void jai_auto_bind(jai::dynamic_binder<T>&, const C&)
// must_not_compile 24: JAI_METHOD: an option is jai::nodiscard, jai::serial or a permission enum
// must_not_compile 25: a && member cannot be called on a script receiver
// must_not_compile 26: default arguments
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/properties.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/core/registrar.hpp>
#include <jaiscript/core/overload.hpp>
#include <jaiscript/serialization/construct.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace must_not_compile {

class ForgottenFriendSerialize {
	int capacity = 20;
	template<class Archive> void serialize(Archive& a_archive) { a_archive(JAI_NVP(capacity)); }
};

class ForgottenFriendSaveLoad {
	int capacity = 20;
	template<class Archive> void save(Archive& a_archive) const { a_archive(JAI_NVP(capacity)); }
	template<class Archive> void load(Archive& a_archive) { a_archive(JAI_NVP(capacity)); }
};

class ForgottenFriendConstruct {
public:
	explicit ForgottenFriendConstruct(int a_capacity) : capacity(a_capacity) {}
	template<class Archive> void serialize(Archive& a_archive) { a_archive(JAI_NVP(capacity)); }
private:
	int capacity;
	template<class Archive>
	static void load_and_construct(Archive& a_archive, jai::serialization::construct<ForgottenFriendConstruct>& a_construct) {
		a_construct(0);
		a_construct->serialize(a_archive);
	}
};

class ForgottenFriendRegistered {
public:
	explicit ForgottenFriendRegistered(int a_capacity) : capacity(a_capacity) {}
	virtual ~ForgottenFriendRegistered() = default;
	template<class Archive> void serialize(Archive& a_archive) { a_archive(JAI_NVP(capacity)); }
private:
	int capacity;
	template<class Archive>
	static void load_and_construct(Archive& a_archive, jai::serialization::construct<ForgottenFriendRegistered>& a_construct) {
		a_construct(0);
		a_construct->serialize(a_archive);
	}
};

class InheritsConstruct : public ForgottenFriendRegistered {
public:
	explicit InheritsConstruct(int a_capacity) : ForgottenFriendRegistered(a_capacity) {}
};

class RegisteredBase {
	friend jai::access;
public:
	explicit RegisteredBase(int a_capacity) : capacity(a_capacity) {}
	virtual ~RegisteredBase() = default;
	template<class Archive> void serialize(Archive& a_archive) { a_archive(JAI_NVP(capacity)); }
private:
	int capacity;
	template<class Archive>
	static void load_and_construct(Archive& a_archive, jai::serialization::construct<RegisteredBase>& a_construct) {
		a_construct(0);
		a_construct->serialize(a_archive);
	}
};

class BuiltAtRunTime : public RegisteredBase {
public:
	explicit BuiltAtRunTime(int a_capacity) : RegisteredBase(a_capacity) {}
};

struct NoRoute {
	int capacity = 20;
};

union NoRouteUnion {
	int capacity;
	float ratio;
};

struct NoWayToConstruct {
	explicit NoWayToConstruct(int a_capacity) : capacity(a_capacity) {}
	int capacity;
	template<class Archive> void serialize(Archive& a_archive) { a_archive(JAI_NVP(capacity)); }
};

struct SavesToAPath {
	virtual ~SavesToAPath() = default;
	int capacity = 20;
	void save(const std::string&) const {}
	bool load() { return true; }
};

// a property_owner registers itself where it is defined, so each of these is an error as a declaration
#if JAI_MUST_NOT_COMPILE == 12 || JAI_MUST_NOT_COMPILE == 13
class ForgottenFriendOwner : public jai::property_owner<ForgottenFriendOwner> {
public:
	JAI_PROPERTY((int), capacity, 20);
protected:
	template<class Archive> void save(Archive& a_archive, std::uint32_t const) const { property_mgr.save(a_archive); }
	template<class Archive> void load(Archive& a_archive, std::uint32_t const) { property_mgr.load(a_archive); }
};

#elif JAI_MUST_NOT_COMPILE == 14
class ForgottenFriendLevel : public jai::property_owner<ForgottenFriendLevel> {
public:
	JAI_PROPERTY((int), capacity, 20);
private:
	template<class Archive> void serialize(Archive& a_archive) { a_archive(JAI_NVP(capacity)); }
};
#endif

struct ToolContext {};
struct EditorContext : ToolContext {
	using jai_context_bases = jai::bases<ToolContext>;
};
struct ServerContext {};

class ToolOwner : public jai::property_owner<ToolOwner, jai::context<ToolContext>> {
public:
	ToolOwner() {}
	static void jai_auto_bind(jai::dynamic_binder<ToolOwner>&, const ToolContext&) {}
};
class EditorOwner : public jai::property_owner<EditorOwner, ToolOwner, jai::context<EditorContext>> {
public:
	EditorOwner() {}
};
class ServerOwner : public jai::property_owner<ServerOwner, jai::context<ServerContext>> {
public:
	ServerOwner() {}
};
// an untagged subclass inherits its base's context; a subclass that declares no hook finds its base's, which is no error
class InheritsEditor : public jai::property_owner<InheritsEditor, EditorOwner> {
public:
	InheritsEditor() {}
};
class PlainHooked : public jai::property_owner<PlainHooked> {
public:
	PlainHooked() {}
	static void jai_auto_bind(jai::dynamic_binder<PlainHooked>&) {}
};
class InheritsPlainHook : public jai::property_owner<InheritsPlainHook, PlainHooked> {
public:
	InheritsPlainHook() {}
};

#if JAI_MUST_NOT_COMPILE == 15
class SwitchesContext : public jai::property_owner<SwitchesContext, ToolOwner, jai::context<ServerContext>> {};
#elif JAI_MUST_NOT_COMPILE == 16
class WidensContext : public jai::property_owner<WidensContext, EditorOwner, jai::context<ToolContext>> {};
#elif JAI_MUST_NOT_COMPILE == 17
class TwoContexts : public jai::property_owner<TwoContexts, ToolOwner, ServerOwner> {};
#elif JAI_MUST_NOT_COMPILE == 18
class WrongHookContext : public jai::property_owner<WrongHookContext, jai::context<ToolContext>> {
public:
	WrongHookContext() {}
	static void jai_auto_bind(jai::dynamic_binder<WrongHookContext>&, const ServerContext&) {}
};
#elif JAI_MUST_NOT_COMPILE == 21
class TwoHooks : public jai::property_owner<TwoHooks> {
public:
	TwoHooks() {}
	static void jai_auto_bind(jai::dynamic_binder<TwoHooks>&) {}
	static void jai_auto_bind(jai::dynamic_binder<TwoHooks>&, const ToolContext&) {}
};
#elif JAI_MUST_NOT_COMPILE == 22
class UntaggedContextHook : public jai::property_owner<UntaggedContextHook> {
public:
	UntaggedContextHook() {}
	static void jai_auto_bind(jai::dynamic_binder<UntaggedContextHook>&, const ToolContext&) {}
};
#elif JAI_MUST_NOT_COMPILE == 23
class TaggedPlainHook : public jai::property_owner<TaggedPlainHook, jai::context<ToolContext>> {
public:
	TaggedPlainHook() {}
	static void jai_auto_bind(jai::dynamic_binder<TaggedPlainHook>&) {}
};
#elif JAI_MUST_NOT_COMPILE == 24
class StrayOption : public jai::property_owner<StrayOption> {
public:
	JAI_METHOD(void, poke, (), 42) {}
};
#elif JAI_MUST_NOT_COMPILE == 25
class RvalueMethod : public jai::property_owner<RvalueMethod> {
public:
	RvalueMethod() {}
	JAI_METHOD(void, consume, () &&) {}
};
#elif JAI_MUST_NOT_COMPILE == 26
class DefaultedMethod : public jai::property_owner<DefaultedMethod> {
public:
	JAI_METHOD(void, spawn, (int64_t a_count = 1)) { (void)a_count; }
};
#endif

// a JAI_METHOD of every shape a script reaches, in a property_owner and in a plain class a registrar binds
class Methods : public jai::property_owner<Methods> {
public:
	JAI_PROPERTY((int64_t), count, 0);
	JAI_METHOD(void, add, ()) { count = count.get() + 1; }
	JAI_METHOD(void, add, (int64_t a_amount));
	JAI_METHOD(int64_t, total, () const noexcept) { return count.get(); }
	JAI_METHOD(int64_t, peek, () const &) { return count.get(); }
	JAI_METHOD((std::map<std::string, int64_t>), tally, () const, jai::nodiscard, jai::serial, jai::permissions::filesystem) { return {}; }
	JAI_METHOD(int64_t, operator*, (int64_t a_factor) const) { return count.get() * a_factor; }
private:
	JAI_METHOD(void, touch, ()) {}
};
inline void Methods::add(int64_t a_amount) { count = count.get() + a_amount; }
struct PlainMethods {
	JAI_METHOD(int64_t, read, () const) { return 1; }
};

struct Overloaded {
	int text() const { return 1; }
	int text() { return 2; }
};

template<typename T>
struct Templated {
	T value{};
};

class Owner : public jai::property_owner<Owner> {
public:
	JAI_PROPERTY((ForgottenFriendSerialize), forgotten);
	JAI_PROPERTY((std::vector<NoRoute>), unroutable);
	void save(const std::string&) const {}
	JAI_TRANSIENT_PROPERTY((NoRoute), scratch);
	JAI_TRANSIENT_PROPERTY((std::vector<SavesToAPath>), paths);
	JAI_TRANSIENT_PROPERTY((std::shared_ptr<NoWayToConstruct>), unbuilt);
};

inline std::string exercise(jai::engine& a_engine) {
#if JAI_MUST_NOT_COMPILE == 1
	return jai::to_json(a_engine, ForgottenFriendSerialize{});
#elif JAI_MUST_NOT_COMPILE == 2
	return jai::to_json(a_engine, ForgottenFriendSaveLoad{});
#elif JAI_MUST_NOT_COMPILE == 3
	jai::from_json<ForgottenFriendSaveLoad>(a_engine, "{}");
	return {};
#elif JAI_MUST_NOT_COMPILE == 4
	jai::from_json<std::shared_ptr<ForgottenFriendConstruct>>(a_engine, "{}");
	return {};
#elif JAI_MUST_NOT_COMPILE == 5
	return jai::to_json(a_engine, NoRouteUnion{});
#elif JAI_MUST_NOT_COMPILE == 6
	jai::from_binary<NoRouteUnion>(a_engine, {});
	return {};
#elif JAI_MUST_NOT_COMPILE == 7
	return jai::to_json(a_engine, std::map<std::string, std::vector<NoRouteUnion>>{});
#elif JAI_MUST_NOT_COMPILE == 8
	jai::serialization::json_archive_writer writer(0, &a_engine);
	ForgottenFriendSerialize forgotten;
	writer.begin_object();
	writer(JAI_NVP(forgotten));
	writer.end_object();
	return writer.str();
#elif JAI_MUST_NOT_COMPILE == 9
	static jai::registrar<ForgottenFriendRegistered, void> registered("ForgottenFriendRegistered");
	return {};
#elif JAI_MUST_NOT_COMPILE == 10
	jai::from_json<std::shared_ptr<NoWayToConstruct>>(a_engine, "{}");
	return {};
#elif JAI_MUST_NOT_COMPILE == 11
	static jai::registrar<InheritsConstruct, void, jai::bases<ForgottenFriendRegistered>> registered("InheritsConstruct");
	return {};
#elif JAI_MUST_NOT_COMPILE == 12
	return jai::to_json(a_engine, ForgottenFriendOwner{});
#elif JAI_MUST_NOT_COMPILE == 13
	jai::from_binary<ForgottenFriendOwner>(a_engine, {});
	return {};
#elif JAI_MUST_NOT_COMPILE == 14
	return jai::to_json(a_engine, std::vector<std::shared_ptr<ForgottenFriendLevel>>{});
#elif JAI_MUST_NOT_COMPILE == 18
	return jai::to_json(a_engine, WrongHookContext{});
#elif JAI_MUST_NOT_COMPILE == 21
	return jai::to_json(a_engine, TwoHooks{});
#elif JAI_MUST_NOT_COMPILE == 22
	return jai::to_json(a_engine, UntaggedContextHook{});
#elif JAI_MUST_NOT_COMPILE == 23
	return jai::to_json(a_engine, TaggedPlainHook{});
#elif JAI_MUST_NOT_COMPILE == 25
	return jai::to_json(a_engine, RvalueMethod{});
#elif JAI_MUST_NOT_COMPILE == 19
	constexpr auto text = jai::overload<>(&Overloaded::text);
	return std::to_string((Overloaded{}.*text)());
#elif JAI_MUST_NOT_COMPILE == 20
	jai::dynamic_binder<Templated<int>> binder(a_engine);
	return {};
#else
	constexpr auto text = jai::const_overload<>(&Overloaded::text);
	jai::dynamic_binder<Templated<int>> binder(a_engine, "TemplatedInt");
	static jai::registrar<SavesToAPath, void> boundOnly("SavesToAPath");
	static jai::registrar<BuiltAtRunTime, void, jai::bases<RegisteredBase>> builtAtRunTime("BuiltAtRunTime");
	static jai::registrar<PlainMethods, void> plainMethods("PlainMethods");
	// a class with no route of its own writes and reads as its binding in the archive's engine declares
	jai::from_binary<NoRoute>(a_engine, jai::to_binary(a_engine, NoRoute{}));
	return jai::to_json(a_engine, Owner{}) + jai::to_json(a_engine, SavesToAPath{}) + jai::to_json(a_engine, std::map<std::string, std::vector<NoRoute>>{}) +
		std::to_string((Overloaded{}.*text)()) + jai::to_json(a_engine, InheritsEditor{}) + jai::to_json(a_engine, InheritsPlainHook{}) +
		jai::to_json(a_engine, Methods{});
#endif
}

}
