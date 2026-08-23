#!/usr/bin/env python3
"""Prove the demo's GLSL is the plugin's GLSL.

The rule the demo pages are built on is that the shader text is *copied*, not
rewritten — the kit's `port()` handles the version line and the precision
qualifiers and nothing else. `demo/tools/sync_shaders.py` does the copying here,
which is one step better than the rest of the fleet's by-hand version — but a
generator that has not been RUN is exactly as stale as a copy that has not been
updated, and only this can tell.

That matters more here than on most of these pages. This plugin's claim is that
two of its settings return the picture untouched, and the demo carries both of
them as presets for a visitor to check. A demo running a stale shader would be
evidence for a claim about a shader that no longer exists.

This reads both and compares them character for character.

    python3 demo/tools/check_shaders.py

Exit status is 0 when every pass matches, 1 otherwise, so it can go in
`tools/verify.sh`. The only edits it allows for are the two JavaScript template
literal escapes — a backslash before a backtick or a `${` — which are required
by the file format and change no GLSL.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHADERS_CPP = ROOT / "source" / "Shaders.cpp"
PLUGIN_JS = ROOT / "demo" / "plugin.js"
INDEX_HTML = ROOT / "demo" / "index.html"
CMAKELISTS = ROOT / "CMakeLists.txt"

# C++ name -> JavaScript name.
PASSES = {
    "kVertexShader": "VERTEX",
    "kFragmentShader": "FRAGMENT",
}


def cpp_literals(text):
    """Every `const char* const kName = R"(...)";` in the file."""
    pattern = re.compile(r'const char\* const (\w+)\s*=\s*R"\((.*?)\)";', re.DOTALL)
    return {m.group(1): m.group(2) for m in pattern.finditer(text)}


def js_literals(text):
    """Every top-level ``const NAME = `...`;`` in the file."""
    pattern = re.compile(r"^const (\w+) = `(.*?)`;$", re.DOTALL | re.MULTILINE)
    return {m.group(1): m.group(2) for m in pattern.finditer(text)}


def unescape(source):
    """Undo the two escapes a template literal forces, and nothing else."""
    return source.replace("\\`", "`").replace("\\${", "${")


def main():
    cpp = cpp_literals(SHADERS_CPP.read_text(encoding="utf-8"))
    js = js_literals(PLUGIN_JS.read_text(encoding="utf-8"))

    failures = 0

    for cpp_name, js_name in PASSES.items():
        if cpp_name not in cpp:
            print(f"MISSING  {cpp_name} not found in source/Shaders.cpp")
            failures += 1
            continue
        if js_name not in js:
            print(f"MISSING  {js_name} not found in demo/plugin.js")
            failures += 1
            continue

        want = cpp[cpp_name]
        got = unescape(js[js_name])

        if want == got:
            print(f"ok       {cpp_name} == {js_name}  ({len(want)} chars)")
            continue

        failures += 1
        print(f"DRIFTED  {cpp_name} != {js_name}")

        want_lines = want.splitlines()
        got_lines = got.splitlines()
        for i in range(max(len(want_lines), len(got_lines))):
            a = want_lines[i] if i < len(want_lines) else "<end of file>"
            b = got_lines[i] if i < len(got_lines) else "<end of file>"
            if a != b:
                print(f"           line {i + 1}")
                print(f"           Shaders.cpp: {a!r}")
                print(f"           plugin.js:   {b!r}")
                break

    # And the version the page reports itself as, which is what a feedback
    # report carries. There is no build step here, so it is stamped into the
    # markup by sync_shaders.py -- which means a version bump leaves it stale,
    # and this is the only thing that would say so.
    version = re.search(
        r"^\s*VERSION\s+([0-9][0-9.]*)\s*$", CMAKELISTS.read_text(encoding="utf-8"), re.MULTILINE
    )
    stamped = re.search(r'data-version="([^"]*)"', INDEX_HTML.read_text(encoding="utf-8"))

    if version is None:
        print("MISSING  no VERSION in CMakeLists.txt")
        failures += 1
    elif stamped is None:
        print("MISSING  demo/index.html has no data-version — run demo/tools/sync_shaders.py")
        failures += 1
    elif stamped.group(1) != version.group(1):
        print(
            f"DRIFTED  demo/index.html says {stamped.group(1)}, CMakeLists says "
            f"{version.group(1)} — run demo/tools/sync_shaders.py"
        )
        failures += 1
    else:
        print(f"ok       data-version {version.group(1)}")

    if failures:
        print(f"\n{failures} check(s) failed. The demo is not the plugin it says it is.")
        return 1

    print(f"\nall {len(PASSES)} pass(es) identical, and the version matches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
