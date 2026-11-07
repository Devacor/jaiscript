#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/signals/signal.hpp>
#include <jaiscript/signals/signal_impl.hpp>
#include <jaiscript/signals/signal_binding.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/signals/signal_serialization.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <vector>
#include <stdexcept>
#include <thread>
#include <atomic>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class emit_board {
public:
    jai::signal_emitter<void(int)> onTickedSignal;
    jai::signal<void(int)> onTicked{ onTickedSignal };
};

class gate_board {
public:
    jai::signal_emitter<bool(int)> onAskSignal;
    jai::signal<bool(int)> onAsk{ onAskSignal };
};

struct signal_box {
    signal_emitter<void(int)> sig;
    template<typename Archive> void serialize(Archive& ar) { ar("sig", sig); }
};

class signal_tests : public suite {
public:
    signal_tests() : suite("Signals") {}

    void forge_tests() override {
        test("receiver_cpp_callback", [this]() {
            int called = 0;
            int received_value = 0;

            auto recv = receiver<void(int)>::make([&](int x) {
                called++;
                received_value = x;
            });

            recv->notify(42);
            check_eq(called, 1, "Receiver called once");
            check_eq(received_value, 42, "Receiver received correct value");

            recv->notify(100);
            check_eq(called, 2, "Receiver called twice");
            check_eq(received_value, 100, "Receiver received updated value");
        });

        test("receiver_block_unblock", [this]() {
            int called = 0;
            auto recv = receiver<void()>::make([&]() { called++; });

            recv->notify();
            check_eq(called, 1, "Unblocked receiver fires");

            recv->block();
            recv->notify();
            check_eq(called, 1, "Blocked receiver doesn't fire");

            recv->unblock();
            recv->notify();
            check_eq(called, 2, "Unblocked receiver fires again");
        });

        test("signal_emitter_connect_emit", [this]() {
            signal_emitter<void(int, int)> sig;

            int sum = 0;
            auto recv = sig.connect([&](int a, int b) {
                sum = a + b;
            });

            sig.emit(10, 20);
            check_eq(sum, 30, "Signal emitted to receiver");
        });

        // A throwing slot must not permanently wedge the emitter: the RAII depth guard restores
        // depth (so later disconnects take effect immediately) and drains even when emit throws.
        test("emit_not_wedged_after_slot_throws", [this]() {
            signal_emitter<void()> sig;

            int fired = 0;
            auto bad = sig.connect([]() { throw std::runtime_error("boom"); });
            auto good = sig.connect([&fired]() { fired++; });  // hold the receiver alive

            try { sig.emit(); } catch (...) {}  // slot throws; emitter must stay usable

            sig.disconnect(bad);  // must take effect now, not be stuck in the deferred queue

            fired = 0;
            sig.emit();  // would re-throw (bad still connected) if the emitter were wedged
            check_eq(1, fired, "remaining receiver fires after a prior slot threw");
        });

        // A slot that re-emits the SAME signal (reentrancy) must not corrupt iteration via a
        // bool in_call_; the depth counter defers cleanup to the outermost emit only.
        test("reentrant_emit_is_safe", [this]() {
            signal_emitter<void(int)> sig;
            int total = 0;
            auto recv = sig.connect([&](int x) {
                total += x;
                if (x > 0) sig.emit(x - 1);
            });
            sig.emit(3);
            check_eq(3 + 2 + 1 + 0, total, "reentrant emits all fire without corruption");
        });

        test("signal_emitter_multiple_receivers", [this]() {
            signal_emitter<void(int)> sig;

            std::vector<int> results;
            auto recv1 = sig.connect([&](int x) { results.push_back(x * 1); });
            auto recv2 = sig.connect([&](int x) { results.push_back(x * 2); });
            auto recv3 = sig.connect([&](int x) { results.push_back(x * 3); });

            sig.emit(10);
            check_eq(results.size(), 3u, "All three receivers called");
            // Results may be in any order due to set ordering
            int total = 0;
            for (int r : results) total += r;
            check_eq(total, 60, "All receivers processed correctly"); // 10 + 20 + 30
        });

        test("signal_emitter_disconnect", [this]() {
            signal_emitter<void()> sig;

            int count1 = 0, count2 = 0;
            auto recv1 = sig.connect([&]() { count1++; });
            auto recv2 = sig.connect([&]() { count2++; });

            sig.emit();
            check_eq(count1, 1, "First receiver called");
            check_eq(count2, 1, "Second receiver called");

            sig.disconnect(recv1);
            sig.emit();
            check_eq(count1, 1, "Disconnected receiver not called");
            check_eq(count2, 2, "Remaining receiver still called");
        });

        test("signal_emitter_owned_connections", [this]() {
            signal_emitter<void()> sig;

            int count = 0;
            sig.connect("my_handler", [&]() { count++; });

            check_true(sig.connected("my_handler"), "Owned connection exists");

            sig.emit();
            check_eq(count, 1, "Owned connection fires");

            auto conn = sig.connection("my_handler");
            check_true(conn != nullptr, "Can retrieve owned connection");

            sig.disconnect("my_handler");
            check_false(sig.connected("my_handler"), "Owned connection removed");

            sig.emit();
            check_eq(count, 1, "Disconnected owned connection doesn't fire");
        });

        test("signal_emitter_block_unblock", [this]() {
            signal_emitter<void()> sig;

            int count = 0;
            auto recv = sig.connect([&]() { count++; });

            sig.emit();
            check_eq(count, 1, "Unblocked signal fires");

            sig.block();
            sig.emit();
            check_eq(count, 1, "Blocked signal doesn't fire");

            bool was_called = sig.unblock();
            check_true(was_called, "Signal reports it was called while blocked");

            sig.emit();
            check_eq(count, 2, "Unblocked signal fires again");
        });

        test("signal_emitter_weak_reference", [this]() {
            signal_emitter<void()> sig;

            int count = 0;
            {
                auto recv = sig.connect([&]() { count++; });
                sig.emit();
                check_eq(count, 1, "Receiver fires while alive");
            }
            // recv is out of scope, weak_ptr should expire

            sig.emit();
            check_eq(count, 1, "Dead receiver doesn't fire");

            // Cull should remove dead observers
            size_t remaining = sig.cull_dead_observers();
            check_eq(remaining, 0u, "No observers remain after cull");
        });

        test("public_signal_connect_only", [this]() {
            // Simulate the Button pattern
            struct MockButton {
                signal_emitter<void(int, int)> on_click_;
                signal<void(int, int)> on_click{on_click_};

                void click(int x, int y) {
                    on_click_.emit(x, y);
                }
            };

            MockButton btn;
            int click_x = 0, click_y = 0;

            // Connect via public signal interface
            auto recv = btn.on_click.connect([&](int x, int y) {
                click_x = x;
                click_y = y;
            });

            btn.click(100, 200);
            check_eq(click_x, 100, "Click x coordinate received");
            check_eq(click_y, 200, "Click y coordinate received");
        });

        test("signal_script_callback", [this]() {
            auto eng = jai::foundry::make_engine();

            int result = 0;
            eng->add_global_ref("result", result);

            signal_emitter<void(int)> sig;
            sig.script_engine(eng.get());
            sig.parameter_names({"value"});

            auto recv = sig.connect("result = value * 2;");

            sig.emit(21);
            check_eq(result, 42, "Script callback executed correctly");
        });

        // jaidoom netplay finding (d): "nested-element pushes from signal receivers
        // don't land" — the mutations always landed; a receiver-body ERROR was being
        // swallowed by call_script's silent catch, which is indistinguishable from a
        // lost mutation at the consumer. Pins: pushes from BOTH receiver flavors
        // mutate live globals, and receiver errors throw out of the emit.
        test("signal_receiver_container_pushes_land", [this]() {
            auto eng = jai::foundry::make_engine();
            eng->execute(R"(
                var G = {"lists": [[]], "flat": []};
                function on_evt(int v) -> void {
                    G["flat"].push(v);
                    G["lists"][0].push(v * 10);
                }
            )");
            signal_emitter<void(int)> sig;
            sig.script_engine(eng.get());
            sig.parameter_names({"v"});
            auto r1 = sig.connect("fn_recv", eng->get_variable("on_evt"));
            auto r2 = sig.connect("G[\"lists\"][0].push(v + 1);");
            sig.emit(7);
            check_eq((int64_t)1, eng->execute("G[\"flat\"].size()").as_int(), "flat push landed");
            check_eq((int64_t)2, eng->execute("G[\"lists\"][0].size()").as_int(),
                "nested pushes landed from both receiver flavors");
        });

        // A script receiver's failure throws out of the emit with the text a direct call gives (a text receiver: execute of
        // its text; a function receiver: its converted std::function); the receivers after it do not run
        test("script_receiver_errors_throw_out_of_the_emit", [this]() {
            auto eng = jai::foundry::make_engine();
            jai::stdlib::register_all(*eng);
            std::vector<std::string> reported;
            eng->set_script_error_handler([&](const std::string& msg) { reported.push_back(msg); });
            eng->execute(R"(
                int seen = 0;
                var divides = [](int v) -> void { int z = v / 0; seen += 100; };
                var counts = [](int v) -> void { seen += v; };
            )");
            auto thrown_text = [](auto&& fn) {
                try { fn(); } catch (const jai::runtime_error& e) { return std::string(e.what()); }
                return std::string("<no throw>");
            };

            signal_emitter<void(int)> text_sig;
            text_sig.script_engine(eng.get());
            text_sig.parameter_names({"v"});
            auto t1 = text_sig.connect("a_text", std::string("int z = v / 0; seen += 100;"));
            auto t2 = text_sig.connect("b_counts", eng->get_variable("counts"));
            instance_variables locals;
            locals["v"] = eng->make_value(int64_t{3});
            const std::string direct_text = thrown_text([&]() { eng->execute("int z = v / 0; seen += 100;", locals); });
            const std::string text_receiver = thrown_text([&]() { text_sig.emit(3); });
            check_true(direct_text.find("Division by zero") != std::string::npos, direct_text);
            check_eq(direct_text, text_receiver, "a text receiver throws what executing its text throws");
            check_eq((int64_t)0, eng->get_variable("seen").as_int(), "the receiver after the failing one did not run");

            signal_emitter<void(int)> fn_sig;
            auto f1 = fn_sig.connect("a_divides", eng->get_variable("divides"));
            auto f2 = fn_sig.connect("b_counts", eng->get_variable("counts"));
            auto host_fn = convert_script_function<void(int)>(eng->get_variable("divides"));
            const std::string direct_fn = thrown_text([&]() { host_fn(3); });
            const std::string fn_receiver = thrown_text([&]() { fn_sig.emit(3); });
            check_true(direct_fn.find("Division by zero") != std::string::npos, direct_fn);
            check_eq(direct_fn, fn_receiver, "a function receiver throws what its converted std::function throws");
            check_eq((int64_t)0, eng->get_variable("seen").as_int());

            signal_emitter<void(int)> throw_sig;
            throw_sig.script_engine(eng.get());
            throw_sig.parameter_names({"v"});
            auto th = throw_sig.connect("a_throws", std::string("throw \"boom \" + to_string(v);"));
            check_eq(std::string("boom 4"), thrown_text([&]() { throw_sig.emit(4); }), "a script throw arrives with its text");

            signal_emitter<void(int)> once_sig;
            auto once = once_sig.connect_oneshot([](int) { throw jai::runtime_error("once"); });
            check_eq(std::string("once"), thrown_text([&]() { once_sig.emit(1); }));
            check_eq(std::string("<no throw>"), thrown_text([&]() { once_sig.emit(2); }), "a one-shot that threw has fired");

            check_true(reported.empty(), "nothing went to the error handler");
            check_eq((int64_t)3, eng->execute("1 + 2").as_int(), "engine stays usable");
        });

        // A load rebuilds an unnamed text receiver with nothing outside to hold it: the emitter holds it, as it holds a named one
        test("unnamed_text_receivers_survive_a_load", [this]() {
            auto eng = jai::foundry::make_engine();
            eng->execute("int hits = 0;");
            signal_box box;
            box.sig.script_engine(eng.get());
            box.sig.parameter_names({"v"});
            auto held = box.sig.connect(std::string("hits += v;"));
            auto named = box.sig.connect("named", std::string("hits += 100;"));
            auto save = [&](const signal_box& b) {
                serialization::json_archive_writer w(2, eng.get());
                w.begin_object("", 1);
                w.serialize_object_content(b);
                w.end_object();
                return w.str();
            };
            const std::string json = save(box);
            signal_box loaded;
            {
                serialization::json_archive_reader r(json, eng.get());
                std::string type_name;
                uint32_t version = 0;
                r.begin_object(type_name, version);
                r(loaded);
                r.end_object();
            }
            loaded.sig.emit(2);
            check_eq((int64_t)102, eng->get_variable("hits").as_int(), "the unnamed receiver and the named one both ran after the load");
            check_eq(json, save(loaded), "the loaded signal saves what it loaded");
            loaded.sig.clear();
            loaded.sig.emit(5);
            check_eq((int64_t)102, eng->get_variable("hits").as_int(), "clear() drops the loaded receivers");
        });

        test("script_receiver_errors_reach_the_script_that_fired_the_signal", [this]() {
            auto eng = jai::foundry::make_engine();
            jai::stdlib::register_all(*eng);
            signal_emitter<void(int)> text_sig;
            text_sig.script_engine(eng.get());
            text_sig.parameter_names({"v"});
            signal_emitter<void(int)> fn_sig;
            signal_emitter<void(int)> outer;
            signal_emitter<void(int)> inner;
            eng->add_function("fire_text", [&](script_int v) { text_sig.emit(static_cast<int>(v)); });
            eng->add_function("fire_fn", [&](script_int v) { fn_sig.emit(static_cast<int>(v)); });
            eng->add_function("fire_outer", [&](script_int v) { outer.emit(static_cast<int>(v)); });
            eng->add_function("fire_inner", [&](script_int v) { inner.emit(static_cast<int>(v)); });
            eng->execute(R"(
                int seen = 0;
                var lam = [](int v) -> void { throw "lam " + to_string(v); };
                var nests = [](int v) -> void { fire_inner(v); seen += 1000; };
                var counts = [](int v) -> void { seen += v; };
            )");
            auto t1 = text_sig.connect("a_text", std::string("throw \"text \" + to_string(v);"));
            auto t2 = text_sig.connect("b_counts", eng->get_variable("counts"));
            auto f1 = fn_sig.connect("a_lam", eng->get_variable("lam"));
            auto f2 = fn_sig.connect("b_counts", eng->get_variable("counts"));
            auto i1 = inner.connect("a_lam", eng->get_variable("lam"));
            auto o1 = outer.connect("a_nests", eng->get_variable("nests"));
            auto o2 = outer.connect("b_counts", eng->get_variable("counts"));
            auto r = eng->execute(R"(
                string got = "";
                try { fire_text(4); got += "missed"; } catch (e) { got += e.message; }
                got += "|";
                try { fire_fn(5); got += "missed"; } catch (e) { got += e.message; }
                got += "|";
                try { fire_outer(6); got += "missed"; } catch (e) { got += e.message; }
                got + "|" + to_string(seen);
            )");
            check_eq(std::string("text 4|lam 5|lam 6|0"), r.as_string(),
                "each failure reached the script's catch, the nested one outward through both emits, and no later receiver ran");
            bool threw = false;
            try { outer.emit(7); } catch (const jai::runtime_error& e) { threw = std::string(e.what()) == "lam 7"; }
            check_true(threw, "a nested emit's failure leaves the outer C++ emit too");
            check_eq((int64_t)0, eng->get_variable("seen").as_int());
        });

        test("signal_disconnect_during_emit", [this]() {
            signal_emitter<void()> sig;

            int count = 0;
            std::shared_ptr<receiver<void()>> recv_to_disconnect;

            auto recv1 = sig.connect([&]() {
                count++;
                // Disconnect recv2 during emission
                if (recv_to_disconnect) {
                    sig.disconnect(recv_to_disconnect);
                }
            });

            auto recv2 = sig.connect([&]() {
                count += 10;
            });
            recv_to_disconnect = recv2;

            sig.emit();
            // Both should fire on first emit (disconnect is queued)
            check_true(count >= 1, "At least first receiver fired");

            count = 0;
            sig.emit();
            // Only recv1 should fire now
            check_eq(count, 1, "Only first receiver fires after disconnect");
        });

        test("signal_clear", [this]() {
            signal_emitter<void()> sig;

            int count = 0;
            auto recv1 = sig.connect([&]() { count++; });
            auto recv2 = sig.connect([&]() { count++; });
            sig.connect("handler1", [&]() { count++; });

            sig.emit();
            check_eq(count, 3, "All receivers fire");

            sig.clear();
            count = 0;
            sig.emit();
            check_eq(count, 0, "No receivers fire after clear");
        });

        test("connect_oneshot", [this]() {
            signal_emitter<void(int)> sig;

            int call_count = 0;
            int last_value = 0;
            auto recv = sig.connect_oneshot([&](int x) {
                call_count++;
                last_value = x;
            });

            sig.emit(10);
            check_eq(call_count, 1, "One-shot called once");
            check_eq(last_value, 10, "One-shot received value");

            sig.emit(20);
            check_eq(call_count, 1, "One-shot not called again");
            check_eq(last_value, 10, "Value unchanged after second emit");
        });

        test("connect_oneshot_multiple", [this]() {
            signal_emitter<void()> sig;

            int regular_count = 0;
            int oneshot_count = 0;

            auto regular = sig.connect([&]() { regular_count++; });
            auto oneshot = sig.connect_oneshot([&]() { oneshot_count++; });

            sig.emit();
            check_eq(regular_count, 1, "Regular receiver called");
            check_eq(oneshot_count, 1, "One-shot receiver called");

            sig.emit();
            check_eq(regular_count, 2, "Regular receiver called again");
            check_eq(oneshot_count, 1, "One-shot not called again");

            sig.emit();
            check_eq(regular_count, 3, "Regular receiver keeps firing");
            check_eq(oneshot_count, 1, "One-shot still only once");
        });

        test("emit_while_short_circuit", [this]() {
            signal_emitter<bool(int)> sig;

            std::vector<int> called_with;

            auto recv1 = sig.connect([&](int x) -> bool {
                called_with.push_back(x);
                return true;  // Continue
            });
            auto recv2 = sig.connect([&](int x) -> bool {
                called_with.push_back(x * 10);
                return false;  // Stop propagation
            });
            auto recv3 = sig.connect([&](int x) -> bool {
                called_with.push_back(x * 100);
                return true;
            });

            bool result = sig.emit_while(5);
            check_false(result, "emit_while returns false when short-circuited");
            check_eq(called_with.size(), 2u, "Only first two receivers called");
            check_eq(called_with[0], 5, "First receiver got value");
            check_eq(called_with[1], 50, "Second receiver got value");
        });

        test("emit_while_no_short_circuit", [this]() {
            signal_emitter<bool()> sig;

            int count = 0;
            auto recv1 = sig.connect([&]() -> bool { count++; return true; });
            auto recv2 = sig.connect([&]() -> bool { count++; return true; });
            auto recv3 = sig.connect([&]() -> bool { count++; return true; });

            bool result = sig.emit_while();
            check_true(result, "emit_while returns true when all continue");
            check_eq(count, 3, "All receivers called");
        });

        test("observer_count", [this]() {
            signal_emitter<void()> sig;

            check_eq(sig.observer_count(), 0u, "No observers initially");

            auto recv1 = sig.connect([&]() {});
            check_eq(sig.observer_count(), 1u, "One observer after connect");

            auto recv2 = sig.connect([&]() {});
            check_eq(sig.observer_count(), 2u, "Two observers");

            sig.disconnect(recv1);
            check_eq(sig.observer_count(), 1u, "One observer after disconnect");

            recv2.reset();  // Let it expire
            sig.cull_dead_observers();
            check_eq(sig.observer_count(), 0u, "No observers after expiry");
        });

        test("receiver_connected", [this]() {
            signal_emitter<void()> sig;

            auto recv = sig.connect([&]() {});
            check_true(recv->connected(), "Receiver is connected after connect");

            sig.disconnect(recv);
            check_false(recv->connected(), "Receiver not connected after disconnect");
        });

        test("receiver_connected_signal_clear", [this]() {
            signal_emitter<void()> sig;

            auto recv1 = sig.connect([&]() {});
            auto recv2 = sig.connect([&]() {});

            check_true(recv1->connected(), "recv1 connected");
            check_true(recv2->connected(), "recv2 connected");

            sig.clear();

            check_false(recv1->connected(), "recv1 not connected after clear");
            check_false(recv2->connected(), "recv2 not connected after clear");
        });

        test("insertion_order_preserved", [this]() {
            signal_emitter<void()> sig;

            std::vector<int> order;
            auto recv1 = sig.connect([&]() { order.push_back(1); });
            auto recv2 = sig.connect([&]() { order.push_back(2); });
            auto recv3 = sig.connect([&]() { order.push_back(3); });

            sig.emit();

            check_eq(order.size(), 3u, "All receivers called");
            check_eq(order[0], 1, "First connected fires first");
            check_eq(order[1], 2, "Second connected fires second");
            check_eq(order[2], 3, "Third connected fires third");
        });

        // Emission rights ride the registered TYPE: signal_emitter<Sig> carries
        // emit(args...), signal<Sig> stays the non-emittable consumer view — which
        // member a class exposes decides what script may do with it.
        test("script_emit_through_bound_emitter_fires_script_receiver", [this]() {
            auto eng = jai::foundry::make_engine();
            bind_signal_type<void(int)>(*eng, "SignalInt");
            dynamic_binder<emit_board>(*eng, "EmitBoard")
                .constructor<>()
                .property("onTicked", &emit_board::onTicked)
                .property("onTickedEmitter", &emit_board::onTickedSignal)
                .build();
            eng->execute("var got = -1; var s = new EmitBoard();");
            eng->execute("s.onTicked.connect(\"t\", [](var v) { got = v; });");
            eng->execute("s.onTickedEmitter.emit(23);");
            check_eq((int64_t)23, eng->execute("got").as_int());
        });

        test("script_emit_through_bound_emitter_fires_cpp_receiver", [this]() {
            auto eng = jai::foundry::make_engine();
            bind_signal_type<void(int)>(*eng, "SignalInt");
            dynamic_binder<emit_board>(*eng, "EmitBoard")
                .constructor<>()
                .property("onTickedEmitter", &emit_board::onTickedSignal)
                .build();
            auto board = std::make_shared<emit_board>();
            int got = -1;
            auto rcv = board->onTicked.connect([&got](int v) { got = v; });
            eng->add_global("s", eng->make_object(board));
            eng->execute("s.onTickedEmitter.emit(31);");
            check_eq(31, got);
        });

        test("signal_view_stays_non_emittable", [this]() {
            auto eng = jai::foundry::make_engine();
            bind_signal_type<void(int)>(*eng, "SignalInt");
            dynamic_binder<emit_board>(*eng, "EmitBoard")
                .constructor<>()
                .property("onTicked", &emit_board::onTicked)
                .build();
            eng->execute("var s = new EmitBoard();");
            check_throws([&]() { eng->execute("s.onTicked.emit(1);"); },
                "the consumer view grants no emission rights");
        });

        // A receiver may destroy the emitter: the emit stops there and returns normally (the emit state outlives the emitter)
        test("a_receiver_destroying_the_emitter_ends_the_emit", [this]() {
            auto board = std::make_unique<emit_board>();
            int later = 0;
            auto kills = board->onTicked.connect([&](int) { board.reset(); });
            auto after = board->onTicked.connect([&](int) { ++later; });
            board->onTickedSignal.emit(1);
            check_true(board == nullptr, "the receiver destroyed the emitter");
            check_eq(0, later, "the receivers after it do not run");
            check_false(kills->connected(), "its receivers are disconnected once the emit unwinds");

            auto nested = std::make_unique<emit_board>();
            int depth = 0;
            auto reenter = nested->onTicked.connect([&](int v) {
                ++depth;
                if (v > 0) { nested->onTickedSignal.emit(v - 1); } else { nested.reset(); }
            });
            nested->onTickedSignal.emit(3);
            check_eq(4, depth, "a reentrant emit destroying the emitter unwinds every level");

            signal_emitter<bool(int)>* gate = new signal_emitter<bool(int)>();
            auto stop = gate->connect([&](int) { delete gate; gate = nullptr; return true; });
            auto never = gate->connect([&](int) { ++later; return true; });
            check_true(gate->emit_while(1), "emit_while returns the destroying receiver's verdict");
            check_eq(0, later);

            auto blocked = std::make_unique<emit_board>();
            blocked->onTickedSignal.set_blocked_callback([&](int) { blocked.reset(); });
            blocked->onTickedSignal.block();
            blocked->onTickedSignal.emit(1);
            check_true(blocked == nullptr, "the blocked callback may destroy the emitter too");
        });

        test("a_script_receiver_dropping_the_last_reference_ends_the_emit", [this]() {
            auto eng = jai::foundry::make_engine();
            bind_signal_type<void(int)>(*eng, "SignalInt");
            dynamic_binder<emit_board>(*eng, "EmitBoard")
                .constructor<>()
                .property("onTicked", &emit_board::onTicked)
                .property("onTickedEmitter", &emit_board::onTickedSignal)
                .build();
            auto board = std::make_shared<emit_board>();
            std::weak_ptr<emit_board> watch = board;
            eng->add_global("s", eng->make_object(board));
            eng->execute("int seen = 0; s.onTicked.connect(\"drop\", [](var v) { s = null; }); s.onTicked.connect(\"count\", [](var v) { seen += 1; });");
            emit_board* raw = board.get();
            board.reset();
            raw->onTickedSignal.emit(5);
            check_true(watch.expired(), "the script dropped the last reference mid-emit");
            check_eq((int64_t)0, eng->execute("seen").as_int());
            check_eq((int64_t)3, eng->execute("1 + 2").as_int(), "engine stays usable");
        });

        test("a_oneshot_fires_once_when_it_reemits", [this]() {
            signal_emitter<void(int)> sig;
            int fired = 0;
            int plain = 0;
            auto once = sig.connect_oneshot([&](int v) { ++fired; if (v < 5) { sig.emit(v + 1); } });
            auto every = sig.connect([&](int) { ++plain; });
            sig.emit(1);
            check_eq(1, fired, "the nested emit skips the one-shot that is firing");
            check_eq(2, plain, "the nested emit reaches the other receivers");
            sig.emit(1);
            check_eq(1, fired);
            check_false(once->connected());
        });

        // A fired one-shot leaves while dropped receivers sit before it in the list (the cleanup erased past its own index)
        test("a_oneshot_after_dropped_receivers_leaves_cleanly", [this]() {
            signal_emitter<void(int)> sig;
            int fired = 0;
            auto first = sig.connect([](int) {});
            auto second = sig.connect([](int) {});
            auto once = sig.connect_oneshot([&](int) { ++fired; });
            auto last = sig.connect([](int) {});
            first.reset();
            second.reset();
            sig.emit(1);
            sig.emit(1);
            check_eq(1, fired);
            check_eq((size_t)1, sig.observer_count());
        });

        test("a_oneshot_an_emit_while_stopped_short_of_still_fires", [this]() {
            signal_emitter<bool(int)> sig;
            int fired = 0;
            auto stop = sig.connect([](int) { return false; });
            auto once = sig.connect_oneshot([&](int) { ++fired; return true; });
            check_false(sig.emit_while(1));
            check_eq(0, fired);
            check_true(once->connected(), "it has not fired, so it stays");
            stop.reset();
            check_true(sig.emit_while(1));
            check_eq(1, fired);
            check_true(sig.emit_while(1));
            check_eq(1, fired);
        });

        test("a_receiver_disconnected_during_an_emit_does_not_run_later_in_it", [this]() {
            std::vector<std::string> order;
            signal_emitter<void(int)> by_handle;
            std::shared_ptr<receiver<void(int)>> later;
            auto first = by_handle.connect([&](int) { order.push_back("first"); by_handle.disconnect(later); });
            later = by_handle.connect([&](int) { order.push_back("later"); });
            by_handle.emit(1);
            check_eq((size_t)1, order.size(), "disconnect(handle) stops a receiver the pass has not reached");
            check_false(later->connected());
            by_handle.emit(1);
            check_eq((size_t)2, order.size());

            order.clear();
            signal_emitter<void(int)> by_name;
            auto front = by_name.connect([&](int) { order.push_back("front"); by_name.disconnect("named"); });
            by_name.connect("named", [&](int) { order.push_back("named"); });
            by_name.emit(1);
            check_eq((size_t)1, order.size(), "disconnect(id) stops a receiver the pass has not reached");
            check_false(by_name.connected("named"));

            order.clear();
            signal_emitter<void(int)> cleared;
            auto clears = cleared.connect([&](int) { order.push_back("clears"); cleared.clear(); });
            auto after = cleared.connect([&](int) { order.push_back("after"); });
            cleared.connect("owned", [&](int) { order.push_back("owned"); });
            cleared.emit(1);
            check_eq((size_t)1, order.size(), "clear() stops every receiver the pass has not reached");
            check_false(after->connected());
            cleared.emit(1);
            check_eq((size_t)1, order.size());
            check_eq((size_t)0, cleared.observer_count());

            order.clear();
            signal_emitter<void(int)> itself;
            std::shared_ptr<receiver<void(int)>> self_handle;
            self_handle = itself.connect([&](int v) {
                order.push_back("self");
                itself.disconnect(self_handle);
                if (v > 0) { itself.emit(v - 1); }
            });
            itself.emit(3);
            check_eq((size_t)1, order.size(), "a receiver that disconnects itself does not run in the nested emit it starts");

            signal_emitter<void(int)> nested;
            std::shared_ptr<receiver<void(int)>> third;
            int third_runs = 0;
            auto opener = nested.connect([&](int v) { if (v == 0) { nested.disconnect(third); nested.emit(1); } });
            auto middle = nested.connect([](int) {});
            third = nested.connect([&](int) { ++third_runs; });
            nested.emit(0);
            check_eq(0, third_runs, "neither the nested emit nor the outer one runs a receiver disconnected before them");
        });

        test("a_disconnect_and_reconnect_during_an_emit_keeps_the_reconnection", [this]() {
            signal_emitter<void(int)> sig;
            std::shared_ptr<receiver<void(int)>> moved;
            int moved_runs = 0;
            bool toggled = false;
            auto toggler = sig.connect([&](int) {
                if (!toggled) {
                    toggled = true;
                    sig.disconnect(moved);
                    sig.connect(moved);
                }
            });
            moved = sig.connect([&](int) { ++moved_runs; });
            sig.emit(1);
            check_eq(0, moved_runs, "the reconnection is a new connection: it joins from the next emit");
            check_true(moved->connected());
            sig.emit(1);
            check_eq(1, moved_runs, "the cleanup after the first emit did not remove the reconnection");
            check_eq((size_t)2, sig.observer_count());
        });

        test("a_oneshot_or_named_receiver_dropped_during_an_emit_does_not_run_later_in_it", [this]() {
            signal_emitter<void(int)> sig;
            std::shared_ptr<receiver<void(int)>> once;
            int once_runs = 0;
            int replaced_runs = 0;
            int replacement_runs = 0;
            bool acted = false;
            auto first = sig.connect([&](int) {
                if (acted) { return; }
                acted = true;
                sig.disconnect(once);
                sig.connect("slot", [&](int) { ++replacement_runs; });
            });
            once = sig.connect_oneshot([&](int) { ++once_runs; });
            sig.connect("slot", [&](int) { ++replaced_runs; });
            sig.emit(1);
            check_eq(0, once_runs, "a one-shot disconnected before the pass reached it never fires");
            check_eq(0, replaced_runs, "a named receiver whose name was taken over does not run");
            check_eq(0, replacement_runs, "the receiver that took the name over joins from the next emit");
            sig.emit(1);
            check_eq(0, once_runs);
            check_eq(0, replaced_runs);
            check_eq(1, replacement_runs);
        });

        test("a_script_receiver_disconnected_during_an_emit_does_not_run_later_in_it", [this]() {
            auto eng = jai::foundry::make_engine();
            eng->execute("int text_runs = 0; int fn_runs = 0; var counts = [](int v) -> void { fn_runs += 1; };");
            signal_emitter<void(int)> sig;
            sig.script_engine(eng.get());
            sig.parameter_names({"v"});
            auto first = sig.connect([&](int) { sig.disconnect("text"); sig.disconnect("fn"); });
            sig.connect("text", std::string("text_runs += 1;"));
            sig.connect("fn", eng->get_variable("counts"));
            sig.emit(1);
            check_eq((int64_t)0, eng->get_variable("text_runs").as_int());
            check_eq((int64_t)0, eng->get_variable("fn_runs").as_int());
        });

        test("cull_dead_observers_during_an_emit_skips_no_live_receiver", [this]() {
            signal_emitter<void(int)> sig;
            std::vector<int> order;
            auto dropped = sig.connect([&](int) { order.push_back(0); });
            auto culls = sig.connect([&](int) { order.push_back(1); sig.cull_dead_observers(); });
            auto last = sig.connect([&](int) { order.push_back(2); });
            dropped.reset();
            sig.emit(1);
            check_eq((size_t)2, order.size(), "the receiver after the cull still ran");
            check_eq(2, order.back());
            check_eq((size_t)2, sig.cull_dead_observers());
        });

        test("a_receiver_connected_during_an_emit_joins_from_the_next_emit", [this]() {
            signal_emitter<void(int)> sig;
            std::vector<std::string> order;
            std::vector<std::shared_ptr<receiver<void(int)>>> added;
            auto grows = sig.connect([&](int) {
                order.push_back("grows");
                added.push_back(sig.connect([&](int) { order.push_back("added"); }));
            });
            sig.emit(1);
            check_eq((size_t)1, order.size(), "a receiver that connects another on every fire ends its emit");
            order.clear();
            sig.emit(1);
            check_eq((size_t)2, order.size(), "the receiver connected during the first emit runs from the second");
            check_eq(std::string("added"), order[1]);

            signal_emitter<void(int)> reentrant;
            int late_runs = 0;
            std::shared_ptr<receiver<void(int)>> late;
            auto starter = reentrant.connect([&](int v) {
                if (v == 0) {
                    late = reentrant.connect([&](int) { ++late_runs; });
                    reentrant.emit(1);
                }
            });
            reentrant.emit(0);
            check_eq(1, late_runs, "a nested emit started after the connect is a new emit and runs it; the outer emit does not");
        });

        test("set_blocked_callback_inside_the_blocked_callback_is_safe", [this]() {
            signal_emitter<void(int)> sig;
            std::string seen;
            const std::string label = "a label long enough to live outside any small buffer";
            sig.set_blocked_callback([&sig, &seen, label](int) {
                sig.set_blocked_callback([](int) {});
                seen = label;
            });
            sig.block();
            sig.emit(1);
            check_eq(label, seen, "the replaced callback finished with its captures intact");
        });

        test("loading_into_a_live_signal_replaces_its_saved_receivers", [this]() {
            auto eng = jai::foundry::make_engine();
            eng->execute("int hits = 0;");
            signal_box box;
            box.sig.script_engine(eng.get());
            box.sig.parameter_names({"v"});
            int cpp_hits = 0;
            auto cpp = box.sig.connect([&](int) { ++cpp_hits; });
            auto unnamed = box.sig.connect(std::string("hits += v;"));
            box.sig.connect("named", std::string("hits += 100;"));
            serialization::json_archive_writer w(2, eng.get());
            w.begin_object("", 1);
            w.serialize_object_content(box);
            w.end_object();
            const std::string json = w.str();
            for (int pass = 0; pass < 2; ++pass) {
                serialization::json_archive_reader r(json, eng.get());
                std::string type_name;
                uint32_t version = 0;
                r.begin_object(type_name, version);
                r(box);
                r.end_object();
            }
            box.sig.emit(1);
            check_eq((int64_t)101, eng->get_variable("hits").as_int(), "each saved receiver runs once after two loads into the same signal");
            check_eq(1, cpp_hits, "the C++ receiver stays connected");
            check_false(unnamed->connected(), "the load replaced the text receiver C++ held with the saved one");
        });

        test("connect_immediate_joins_every_emit_on_the_stack", [this]() {
            signal_emitter<void(int)> sig;
            std::vector<std::string> order;
            std::vector<std::shared_ptr<receiver<void(int)>>> held;
            auto outer = sig.connect([&](int v) {
                order.push_back("outer" + std::to_string(v));
                if (v == 0) { sig.emit(1); }
            });
            auto inner = sig.connect([&](int v) {
                order.push_back("inner" + std::to_string(v));
                if (v == 1) {
                    held.push_back(sig.connect_immediate([&](int w) { order.push_back("immediate" + std::to_string(w)); }));
                    held.push_back(sig.connect([&](int w) { order.push_back("plain" + std::to_string(w)); }));
                }
            });
            sig.emit(0);
            const std::vector<std::string> expected { "outer0", "outer1", "inner1", "immediate1", "inner0", "immediate0" };
            check_eq(expected.size(), order.size());
            for (size_t i = 0; i < expected.size() && i < order.size(); ++i) {
                check_eq(expected[i], order[i], "the immediate receiver runs in the nested emit and in the outer one; the plain one in neither");
            }
            order.clear();
            sig.emit(2);
            check_eq((size_t)4, order.size(), "from the next emit both run like any receiver");
        });

        test("connect_immediate_covers_every_connect_form", [this]() {
            auto eng = jai::foundry::make_engine();
            eng->execute("int text_runs = 0; int fn_runs = 0; var counts = [](int v) -> void { fn_runs += v; };");
            signal_emitter<void(int)> sig;
            sig.script_engine(eng.get());
            sig.parameter_names({"v"});
            int named_runs = 0;
            int handle_runs = 0;
            std::shared_ptr<receiver<void(int)>> unnamed_text;
            auto reattached = receiver<void(int)>::make([&](int) { ++handle_runs; });
            bool acted = false;
            auto first = sig.connect([&](int) {
                if (acted) { return; }
                acted = true;
                sig.connect_immediate("named", [&](int) { ++named_runs; });
                sig.connect_immediate("named_text", std::string("text_runs += v;"));
                sig.connect_immediate("named_fn", eng->get_variable("counts"));
                unnamed_text = sig.connect_immediate(std::string("text_runs += 10;"));
                check_true(sig.connect_immediate(reattached));
            });
            sig.emit(1);
            check_eq(1, named_runs);
            check_eq(1, handle_runs);
            check_eq((int64_t)11, eng->get_variable("text_runs").as_int());
            check_eq((int64_t)1, eng->get_variable("fn_runs").as_int());

            signal<void(int)> facade{ sig };
            int facade_runs = 0;
            std::shared_ptr<receiver<void(int)>> via_facade;
            auto connects = facade.connect([&](int v) { if (v == 2) { via_facade = facade.connect_immediate([&](int) { ++facade_runs; }); } });
            sig.emit(2);
            check_eq(1, facade_runs, "the facade's connect_immediate joins the running emit");
        });

        test("connecting_a_receiver_again_moves_it", [this]() {
            signal_emitter<void(int)> first;
            signal_emitter<void(int)> second;
            int runs = 0;
            auto recv = first.connect([&](int) { ++runs; });
            check_true(first.connect(recv), "connecting it again on the same emitter keeps one connection");
            first.emit(1);
            check_eq(1, runs);
            check_true(second.connect(recv));
            first.emit(1);
            check_eq(1, runs, "connected elsewhere, it no longer runs on its first emitter");
            second.emit(1);
            check_eq(2, runs);
            first.disconnect(recv);
            check_true(recv->connected(), "disconnecting it from an emitter it left changes nothing");
            check_eq((size_t)0, first.observer_count());
            check_eq((size_t)1, second.observer_count());
        });

        test("an_emitter_never_moves", [this]() {
            struct sync_door {
                signal_emitter<void(int), signal_synchronized> onOpenSignal;
                signal<void(int), signal_synchronized> onOpen{ onOpenSignal };
            };
            static_assert(!std::is_move_constructible_v<emit_board> && !std::is_move_assignable_v<emit_board>);
            static_assert(!std::is_move_constructible_v<sync_door> && !std::is_move_assignable_v<sync_door>);
            static_assert(std::is_empty_v<signal_unsynchronized::mutex_type>);
            sync_door door;
            int got = 0;
            auto recv = door.onOpen.connect([&](int v) { got = v; });
            door.onOpenSignal(7);
            check_eq(7, got);
        });

        test("a_synchronized_emitter_takes_calls_from_several_threads", [this]() {
            signal_emitter<void(int), signal_synchronized> sig;
            signal<void(int), signal_synchronized> facade{ sig };
            std::atomic<int> runs { 0 };
            auto steady = facade.connect([&](int) { runs.fetch_add(1); });
            constexpr int EMITS = 2000;
            std::atomic<bool> emitting { true };
            std::atomic<bool> first_emitted { false };
            std::thread emitter_thread([&]() {
                for (int i = 0; i < EMITS; ++i) {
                    sig.emit(i);
                    if (i == 0) { first_emitted = true; }
                }
                emitting = false;
            });
            std::thread churn_thread([&]() {
                // a latch: the churn starts once one emit has run with nothing blocked, so that emit's delivery is certain
                while (!first_emitted.load()) { std::this_thread::yield(); }
                int cycles = 0;
                while (emitting.load() || cycles < 100) {
                    auto passing = facade.connect([&](int) {});
                    facade.connect("named", [&](int) {});
                    steady->block();
                    steady->unblock();
                    sig.block();
                    sig.unblock();
                    sig.set_blocked_callback([](int) {});
                    sig.parameter_names({"v"});
                    facade.disconnect(passing);
                    facade.disconnect("named");
                    ++cycles;
                }
            });
            emitter_thread.join();
            churn_thread.join();
            check_true(runs.load() > 0 && runs.load() <= EMITS, "the steady receiver ran on unblocked emits only");
            check_eq((size_t)1, sig.observer_count(), "every receiver the other thread connected has left");
            check_false(steady->blocked());
        });

        // A host may destroy the engine before the emitters its script connected to: script receivers are skipped from then
        // on, host receivers keep firing, and releasing the receivers afterwards runs no script
        test("a_function_receiver_keeps_its_engine_when_the_emitter_rearms_another", [this]() {
            // Re-arming an emitter for another engine (a text connect, an archive load) rebinds its text receivers; a function
            // receiver stays with the engine its function belongs to, the one its lifetime token names, so its quiet test never
            // reads the other engine, which may be loading, on another thread or destroyed
            int noted = 0;
            jai::signal_emitter<void(int)> emitter;
            auto owner = jai::foundry::make_engine();
            owner->add_function("note", [&noted](int v) { noted += v; });
            auto fn_receiver = emitter.connect("fn", owner->execute("[](int v) { note(v); };"));
            {
                auto other = jai::foundry::make_engine();
                emitter.script_engine(other.get());
                check_true(fn_receiver->script_engine() == owner.get(), "the function receiver keeps its own engine");
            }
            emitter.emit(5);
            check_eq(5, noted, "the receiver runs on its own engine after the other engine is gone");
        });

        test("script_receivers_skip_once_their_engine_is_destroyed", [this]() {
            int noted = 0;
            int host = 0;
            auto board = std::make_shared<emit_board>();
            auto gate = std::make_shared<gate_board>();
            auto hostReceiver = board->onTicked.connect([&host](int v) { host += v; });
            {
                auto eng = jai::foundry::make_engine();
                jai::stdlib::register_all(*eng);
                bind_signal_type<void(int)>(*eng, "SignalInt");
                bind_signal_type<bool(int)>(*eng, "SignalAsk");
                dynamic_binder<emit_board>(*eng, "EmitBoard").constructor<>().property("onTicked", &emit_board::onTicked).build();
                dynamic_binder<gate_board>(*eng, "GateBoard").constructor<>().property("onAsk", &gate_board::onAsk).build();
                eng->add_function("note", [&noted](int v) { noted += v; });
                eng->add_global("board", eng->make_object(board));
                eng->add_global("gate", eng->make_object(gate));
                board->onTickedSignal.script_engine(eng.get());
                board->onTickedSignal.parameter_names({ "v" });
                eng->execute(R"(
                    class Ear { int gain = 10; ~Ear() { note(100000); } void hear(int v) { note(v * gain); } }
                    var ear = Ear();
                    var held = new Ear();
                    board.onTicked.connect("lambda", [held](var v) { held.hear(v); });
                    board.onTicked.connect("method", reflect::bind(ear, "hear"));
                    board.onTicked.connect("text", "note(v * 1000);");
                    gate.onAsk.connect("ask", [held](var v) { held.hear(v); return false; });
                )");
                board->onTickedSignal.emit(1);
                check_eq(1000 + 10 + 10, noted, "every script receiver runs while the engine lives");
                check_false(gate->onAskSignal.emit_while(1), "the live script predicate stops the emit");
                check_eq(1030, noted);
            }
            const int atDeath = noted;
            board->onTickedSignal.emit(2);
            check_eq(atDeath, noted, "no script receiver runs after its engine is destroyed");
            check_eq(3, host, "host receivers keep firing");
            check_true(gate->onAskSignal.emit_while(2), "a skipped predicate lets the emit continue, as a blocked one does");
            check_eq(atDeath, noted);

            auto late = board->onTickedSignal.connect("late", "note(1);");
            board->onTickedSignal.emit(3);
            check_eq(atDeath, noted, "a text receiver connected after the engine is gone is skipped too");
            check_eq(6, host);

            board.reset();
            gate.reset();
            late.reset();
            check_eq(atDeath, noted, "releasing the receivers and their captures runs no script");
        });

        // Receivers the host holds past both the engine and the emitter release without reaching the engine
        test("script_receivers_held_past_their_engine_and_emitter_release_quietly", [this]() {
            int noted = 0;
            auto board = std::make_shared<emit_board>();
            signal_emitter<void(int)>::shared_receiver_type lambda;
            signal_emitter<void(int)>::shared_receiver_type text;
            {
                auto eng = jai::foundry::make_engine();
                jai::stdlib::register_all(*eng);
                bind_signal_type<void(int)>(*eng, "SignalInt");
                eng->add_function("note", [&noted](int v) { noted += v; });
                dynamic_binder<emit_board>(*eng, "EmitBoard").constructor<>().property("onTicked", &emit_board::onTicked).build();
                eng->add_global("board", eng->make_object(board));
                board->onTickedSignal.script_engine(eng.get());
                board->onTickedSignal.parameter_names({ "v" });
                eng->execute(R"(
                    class Ear { ~Ear() { note(100000); } void hear(int v) { note(v); } }
                    var held = new Ear();
                    var nested = [held](int v) { return [held, v]() { held.hear(v); }; };
                    board.onTicked.connect("lambda", [nested](var v) { nested(v)(); });
                )");
                lambda = board->onTicked.connection("lambda");
                text = board->onTickedSignal.connect("note(v * 10);");
                board->onTickedSignal.emit(1);
                check_eq(11, noted);
            }
            const int atDeath = noted;
            board->onTickedSignal.emit(1);
            check_eq(atDeath, noted);
            board.reset();
            check_false(lambda->connected(), "the emitter is gone");
            lambda->notify(5);
            text->notify(5);
            check_eq(atDeath, noted, "a held receiver called directly after its engine is gone runs nothing");
            lambda.reset();
            text.reset();
            check_eq(atDeath, noted, "releasing them runs no script");
        });

        test("a_kept_script_callback_throws_once_its_engine_is_destroyed", [this]() {
            std::function<int(int)> kept;
            {
                auto eng = jai::foundry::make_engine();
                eng->execute("int base = 40; var add = [](int v) { return base + v; };");
                kept = convert_script_function<int(int)>(eng->get_variable("add"));
                check_eq(42, kept(2));
            }
            bool threw = false;
            try {
                kept(2);
            } catch (const jai::runtime_error& e) {
                threw = true;
                check_eq(std::string("Cannot call a script function after its engine was destroyed"), std::string(e.what()));
            }
            check_true(threw, "the call throws to the host instead of reaching the dead engine");
            kept = nullptr;
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::signal_tests);
