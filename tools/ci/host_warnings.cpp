// A host translation unit over every header the embedding pages name, built with -Wall -Wextra -Werror by
// tools/ci/gates.sh headers: a host that builds warning-free can include the library's headers
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/properties.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/properties/observable_property.hpp>
#include <jaiscript/signals/signal_property.hpp>
#include <jaiscript/signals/signal_view.hpp>
#include <jaiscript/core/registrar.hpp>
#include <jaiscript/core/coroutine.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/core/static_binder_impl.hpp>
#include <jaiscript/core/dynamic_binder_serialization.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/stdlib/spider.hpp>
#include <jaiscript/math/adapter.hpp>
#include <jaiscript/debug/connector.hpp>
#include <jaiscript/debug/controller.hpp>
#include <jaiscript/net/http_socket_transport.hpp>
#include <jaiscript/thread_pool.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace host_warnings {

class Widget : public jai::property_owner<Widget> {
	friend jai::access;
public:
	JAI_PROPERTY((std::string), name, "widget");
	JAI_PROPERTY((int64_t), count, 0);
	JAI_PROPERTY((double), scale, 1.5);
	JAI_OBSERVABLE_PROPERTY((int), level, 1);
	JAI_SIGNAL_PROPERTY(void(int64_t), onPoke);
	void poke(int64_t a_amount) { count = count + a_amount; onPokeSignal(count); }
	int64_t total() const { return count; }
	std::string label(const std::string& a_prefix) const { return a_prefix + name.get(); }
	static int64_t twice(int64_t a_value) { return a_value * 2; }
	static std::shared_ptr<Widget> make() { return std::make_shared<Widget>(); }
protected:
	template<class Archive> void save(Archive& archive, std::uint32_t const) const { property_mgr.save(archive); }
	template<class Archive> void load(Archive& archive, std::uint32_t const) { property_mgr.load(archive); }
};

struct Point {
	double x = 0.0;
	double y = 0.0;
	double length() const { return x * x + y * y; }
};

int64_t sum(std::vector<int64_t> a_values) {
	int64_t result = 0;
	for (int64_t value : a_values) { result += value; }
	return result;
}

}

static jai::registrar<host_warnings::Widget, void> registerWidget("Widget", [](jai::dynamic_binder<host_warnings::Widget>& a_binder) {
	a_binder.constructor<>();
	a_binder.method("poke", &host_warnings::Widget::poke);
	a_binder.method("total", &host_warnings::Widget::total);
	a_binder.method("label", &host_warnings::Widget::label);
	a_binder.static_method("twice", &host_warnings::Widget::twice);
	a_binder.static_method("make", &host_warnings::Widget::make);
});

int host_warnings_entry() {
	auto eng = jai::engine::make();
	jai::dynamic_binder<host_warnings::Point>(*eng, "Point")
		.constructor<>()
		.property("x", &host_warnings::Point::x)
		.property("y", &host_warnings::Point::y)
		.method("length", &host_warnings::Point::length)
		.static_method("origin", []() { return host_warnings::Point{}; })
		.build();
	eng->add_function("sum", &host_warnings::sum);
	eng->add_function("shout", [](const std::string& a_text) { return a_text + "!"; });
	eng->add_function("apply", [](std::function<int64_t(int64_t)> a_fn) { return a_fn(2); });
	jai::held_value result = eng->execute("sum([1, 2, 3]) + apply([](int x) { return x + 1; })");
	auto widget = std::make_shared<host_warnings::Widget>();
	std::string json = jai::to_json(*eng, widget);
	auto loaded = jai::from_json<std::shared_ptr<host_warnings::Widget>>(*eng, json);
	return static_cast<int>(result.as_int()) + static_cast<int>(loaded->count.get());
}
