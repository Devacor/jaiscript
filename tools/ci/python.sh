#!/usr/bin/env bash
# Sourced by gates.sh and ci_lib.sh. jai_python prints the first python3 or python on PATH at 3.8 or later, else fails.
jai_python() {
	local candidate
	for candidate in python3 python; do
		"$candidate" -c "import sys; sys.exit(sys.version_info < (3, 8))" >/dev/null 2>&1 && { echo "$candidate"; return 0; }
	done
	echo "::error::the CI scripts need Python 3.8 or later on PATH" >&2
	return 1
}
