#pragma once
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/math_intrinsics.hpp>
#include <limits>

namespace jai::detail {
	inline void register_math_namespace(engine& a_engine) {
		auto& ns = engine_access::script_namespaces(a_engine)[engine_access::symbolize(a_engine, "math")];
		if (!ns) { ns = std::make_shared<script_namespace_data>(); }
		// Function values expose the same kernels that direct math:: calls lower to.
		for (const char* name : {"abs", "acos", "angle_between", "asin", "atan", "atan2", "cbrt", "ceil", "clamp", "color", "cos", "cosh", "cross", "degrees", "degrees_to_radians", "distance", "distance_squared", "dot", "exp", "exp2", "floor", "fmod", "hypot", "ifloor", "is_inf", "is_nan", "itrunc", "ivec2", "ivec3", "length", "length_squared", "lerp", "log", "log10", "log2", "max", "min", "mix", "mix_hsv", "mix_in", "mix_in_out", "mix_out", "mix_out_in", "mix_rgb", "normalize", "perpendicular", "pow", "project", "radians", "radians_to_degrees", "reflect", "remap", "rotate", "round", "saturate", "sign", "sin", "sinh", "sqrt", "tan", "tanh", "trunc", "unmix", "unmix_in", "unmix_in_out", "unmix_out", "unmix_out_in", "vec2", "vec3", "vec4", "wrap", "wrap_add", "wrap_mul", "wrap_sub"}) {
			const math_fn fn = math_intrinsic_from_name(name);
			ns->variables[engine_access::symbolize(a_engine, name)] = script_value::make_function([engine = &a_engine, fn](const std::vector<script_value>& a_args) -> checked_result<script_value> {
				if (a_args.size() > 8) { return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "Too many arguments for math function"); }
				const script_value* arguments[8];
				for (size_t i = 0; i < a_args.size(); ++i) { arguments[i] = &a_args[i].deref(); }
				return eval_math_intrinsic(fn, arguments, a_args.size(), engine);
			}, &a_engine);
		}
		for (const auto& constant : {std::pair{"PI", 3.14159265358979323846}, {"E", 2.71828182845904523536}, {"TAU", 6.28318530717958647692},
			{"INF", std::numeric_limits<script_float>::infinity()}, {"NAN_VALUE", std::numeric_limits<script_float>::quiet_NaN()}}) {
			ns->variables[engine_access::symbolize(a_engine, constant.first)] = script_value(constant.second, &a_engine);
		}
	}
}
