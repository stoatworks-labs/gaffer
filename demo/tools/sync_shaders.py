#!/usr/bin/env python3
"""Copy the plugin's GLSL into the demo, rather than trusting anyone to.

The rule the demo pages are built on is that the shader text is *copied*, not
rewritten — the kit's `port()` handles the version line and the precision
qualifiers and nothing else. Most of the fleet copies it by hand and then checks
for drift; this repo generates it, which is the same discipline one step
earlier.

    python3 demo/tools/sync_shaders.py

Rewrites the `VERTEX` and `FRAGMENT` template literals in `demo/plugin.js` from
`source/Shaders.cpp`. `demo/tools/check_shaders.py` still exists and is still
run by `tools/verify.sh`: a generator that has not been RUN is exactly as stale
as a copy that has not been updated, and only the check can tell.

The two escapes below are the only edits made, and they are forced by the file
format rather than chosen: a backslash before a backtick, and before a `${`.
Neither is GLSL. This shader has 24 backticks in its comments, which is how the
need for them was discovered.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHADERS_CPP = ROOT / "source" / "Shaders.cpp"
PLUGIN_JS = ROOT / "demo" / "plugin.js"

PASSES = {"kVertexShader": "VERTEX", "kFragmentShader": "FRAGMENT"}

INDEX_HTML = ROOT / "demo" / "index.html"
CMAKELISTS = ROOT / "CMakeLists.txt"


def project_version():
    """The version CMake builds, which is the one a bug report should carry."""
    text = CMAKELISTS.read_text(encoding="utf-8")
    match = re.search(r"^\s*VERSION\s+([0-9][0-9.]*)\s*$", text, re.MULTILINE)
    return match.group(1) if match else None


def sync_version():
    """Stamp `data-version` on the support-footer tag.

    Every hosted app in the fleet sets this, so that no feedback report arrives
    saying "Version: not stated". Most of them do it from a build step; this
    page deliberately has none — what is committed is what is served — so it is
    stamped here and CHECKED by check_shaders.py, which `tools/verify.sh` runs.
    A version bump therefore fails verify until this is re-run, which is the
    point: a literal beside the tag is right only until the next release.
    """
    version = project_version()
    if version is None:
        print("MISSING  no VERSION in CMakeLists.txt")
        return 1

    text = INDEX_HTML.read_text(encoding="utf-8")
    wanted = f'data-version="{version}"'

    if wanted in text:
        print(f"ok       data-version already {version}")
        return 0

    if "data-version=" in text:
        updated = re.sub(r'data-version="[^"]*"', wanted, text, count=1)
    else:
        updated = text.replace(
            '      data-app="gaffer"', f'      data-app="gaffer"\n      {wanted}', 1
        )
        if updated == text:
            print("MISSING  no data-app attribute to stamp beside in demo/index.html")
            return 1

    INDEX_HTML.write_text(updated, encoding="utf-8")
    print(f"updated  data-version -> {version}")
    return 0


def escape(source):
    """The two escapes a JavaScript template literal forces, and nothing else."""
    return source.replace("`", "\\`").replace("${", "\\${")


def main():
    cpp = dict(
        re.findall(
            r'const char\* const (\w+)\s*=\s*R"\((.*?)\)";',
            SHADERS_CPP.read_text(encoding="utf-8"),
            re.DOTALL,
        )
    )

    text = PLUGIN_JS.read_text(encoding="utf-8")
    changed = 0

    for cpp_name, js_name in PASSES.items():
        if cpp_name not in cpp:
            print(f"MISSING  {cpp_name} not found in source/Shaders.cpp")
            return 1

        pattern = re.compile(rf"^const {js_name} = `(.*?)`;$", re.DOTALL | re.MULTILINE)
        if not pattern.search(text):
            print(f"MISSING  const {js_name} = `...`; not found in demo/plugin.js")
            return 1

        replacement = f"const {js_name} = `{escape(cpp[cpp_name])}`;"
        updated = pattern.sub(lambda _m: replacement, text, count=1)
        if updated != text:
            changed += 1
            print(f"updated  {js_name}  ({len(cpp[cpp_name])} chars)")
        else:
            print(f"ok       {js_name} already matches")
        text = updated

    PLUGIN_JS.write_text(text, encoding="utf-8")
    print(f"\n{changed} pass(es) rewritten")

    return sync_version()


if __name__ == "__main__":
    sys.exit(main())
