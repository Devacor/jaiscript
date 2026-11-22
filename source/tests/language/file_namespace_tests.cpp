// The runner's file:: namespace: what each function does, its permission, its checked signatures, and a script's own name 'file'.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/type_checker.hpp>
#include "../../../tools/jaiscript/file_io.hpp"

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {

std::shared_ptr<engine> library_engine() {
	auto e = make_engine();
	stdlib::register_all(*e);
	jai_runner::register_file_io(*e);
	return e;
}

std::string run_output(const std::shared_ptr<engine>& e, const std::string& source) {
	auto out = std::make_shared<std::ostringstream>();
	e->set_output_stream(out);
	e->execute(source);
	return out->str();
}

std::string run_output(const std::string& source) { return run_output(library_engine(), source); }

std::string load_errors(const std::shared_ptr<engine>& e, const std::string& source) {
	const auto result = e->check(source);
	return result.has_errors() ? result.format(source) : std::string();
}

std::string load_errors(const std::string& source) { return load_errors(library_engine(), source); }

std::string script_text(const std::filesystem::path& path) { return "\"" + path.generic_string() + "\""; }

} // namespace

class file_namespace_tests : public suite {
public:
	file_namespace_tests() : suite("File Namespace") {}

	void forge_tests() override {
		test("file_namespace_writes_reads_moves_lists_and_removes", [this]() {
			const auto dir = test_temp_directory() / "file_namespace";
			std::filesystem::remove_all(dir);
			const std::string root = script_text(dir);
			check_eq(std::string("true true true\n"), run_output(
				"string root = " + root + ";\n"
				"print(file::ensure_writable_directory(root), \" \", file::make_directories(root + \"/sub\"), \" \", file::directory_exists(root + \"/sub\"));\n"));
			check_eq(std::string("true hello true 5 false\n"), run_output(
				"string a = " + root + " + \"/a.txt\";\n"
				"print(file::write(a, \"hello\"), \" \", file::read(a), \" \", file::exists(a), \" \", file::data(a)[\"size\"], \" \", file::data(a)[\"is_directory\"]);\n"));
			check_eq(std::string("true false true [\"b.txt\", \"sub\"]\n"), run_output(
				"string root = " + root + ";\n"
				"print(file::move(root + \"/a.txt\", root + \"/b.txt\"), \" \", file::exists(root + \"/a.txt\"), \" \", file::exists(root + \"/b.txt\"), \" \", file::list_directory(root));\n"));
			check_eq(std::string("true false true\n"), run_output(
				"string b = " + root + " + \"/b.txt\";\n"
				"print(file::remove(b), \" \", file::exists(b), \" \", file::data(b) == null);\n"));
			std::filesystem::remove_all(dir);
		});

		test("file_functions_keep_the_filesystem_permission", [this]() {
			auto e = library_engine();
			e->disable(permissions::filesystem);
			for (const char* call : {"file::data(\"p\")", "file::read(\"p\")", "file::write(\"p\", \"t\")", "file::exists(\"p\")", "file::remove(\"p\")",
			                         "file::move(\"a\", \"b\")", "file::directory_exists(\"p\")", "file::make_directories(\"p\")",
			                         "file::list_directory(\"p\")", "file::ensure_writable_directory(\"p\")"}) {
				const std::string caught = e->execute("var m = \"\"; try { " + std::string(call) + "; } catch (error) { m = error; } m;").to_string();
				check_eq(std::string("Insufficient permission: filesystem."), caught, call);
			}
		});

		test("file_calls_are_checked_against_their_signatures", [this]() {
			check_true(!load_errors("file::read(1);").empty(), "a number is no path");
			check_true(!load_errors("file::read();").empty(), "a path is required");
			check_true(!load_errors("int n = file::read(\"p\");").empty(), "file::read gives a string");
			check_true(load_errors("string s = file::read(\"p\"); bool b = file::exists(\"p\");").empty(), "declared results");
		});

		test("a_script_may_name_its_own_variable_file", [this]() {
			const auto dir = test_temp_directory() / "file_namespace_variable";
			std::filesystem::create_directories(dir);
			const std::string path = script_text(dir / "note.txt");
			check_eq(std::string("true\nnote\nnote\nnote\nnote\nnote\n"), run_output(
				"string file = " + path + ";\n"
				"print(file::write(file, \"note\"));\n"
				"print(file::read(file));\n"
				"string by_parameter(string file) { return file::read(file); }\n"
				"print(by_parameter(file));\n"
				"for (auto file : [" + path + "]) { print(file::read(file)); }\n"
				"auto by_lambda = [](string file) { return file::read(file); };\n"
				"print(by_lambda(file));\n"
				"class Holder { string file = " + path + "; string read() { return file::read(file); } }\n"
				"print(Holder().read());\n"));
			std::filesystem::remove_all(dir);
		});
	}
};

}   // namespace jai::foundry::tests

using file_namespace_tests = jai::foundry::tests::file_namespace_tests;
FOUNDRY_REGISTER(file_namespace_tests)
