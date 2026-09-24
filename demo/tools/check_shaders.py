"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds nine GLSL pieces and so does `source/Shaders.cpp`. That
is two copies of the same text, and two copies drift -- quietly, because a demo
that renders a *plausible* picture looks exactly like a demo that renders the
right one. The whole claim of these pages is that they run the plugin's own
shader rather than something reimplemented to look similar, so the claim needs
something enforcing it.

Nothing else can. `pltest` drives the real plugin class through a real FFGL
sequence and has no idea this page exists, and `tools/check-shaders.sh`
compiles the C++ copies (through `pltest --dump-shaders`) and never looks at
the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment that has been updated on one side and not the other
is exactly the drift worth catching, because comments in this repo carry the
reasoning that justifies the code: why the pedestal is subtracted at 4.5 sigma,
why the point limit is taken explicitly below a quarter of a sigma, why the
sheet is opaque whatever the clip's alpha.

Nine pieces and not six shaders: the ink pass is ASSEMBLED at run time out of a
shared constants string plus a vertex body and a fragment body, because the
vertex stage sizing the quad and the fragment stage subtracting the Gaussian's
pedestal have to agree about `Extent`. The demo carries the pieces and assembles
them the same way (`InkVertexSource()` / `InkFragmentSource()`), so this checks
the pieces -- a change to the constants would otherwise go unnoticed in both
stages.

The one transformation is a decode, not a normalisation. The ink fragment body
quotes `half` in a comment with backticks, and a backtick cannot appear raw
inside a JavaScript template literal, so `plugin.js` escapes it as \\`. This
unescapes that and *rejects any other backslash on the JS side*; there are none
anywhere in the C++, so a second escape could only be somebody hiding a
difference.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. `Tracer`, `buildStrokes`, `orderStrokes`,
`planBlocks`, `Machine`, `InkRenderer`'s bookkeeping and every `...FromParam` in
plugin.js are a hand translation of Tracer.cpp, Planner.cpp, Machine.cpp,
render/Ink.cpp and Controls.cpp, and only a reader can tell whether they still
agree. When you change one of those, change it here too -- and remember that a
wrong mapping shows up on the page as a picture that is subtly the wrong shape,
which nobody will notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol.
SHADERS = [
    ("VERTEX", "source/Shaders.cpp", "kVertexShader"),
    ("COPY", "source/Shaders.cpp", "kCopyShader"),
    ("EDGE", "source/Shaders.cpp", "kEdgeShader"),
    ("STABILISE", "source/Shaders.cpp", "kStabiliseShader"),
    ("INK_CONSTANTS", "source/Shaders.cpp", "kInkConstants"),
    ("INK_VERTEX_BODY", "source/Shaders.cpp", "kInkVertexBody"),
    ("INK_FRAGMENT_BODY", "source/Shaders.cpp", "kInkFragmentBody"),
    ("RESAMPLE", "source/Shaders.cpp", "kResampleShader"),
    ("COMPOSITE", "source/Shaders.cpp", "kCompositeShader"),
]


def from_cpp(path, symbol):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    if match is None:
        return None
    return match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # Undo the one escape the literal needs, and refuse the rest. The C++ carries
    # no backslash at all, so a stray one here is either a typo or a difference
    # being smuggled through the decoder.
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    print()
    if problems:
        print(f"{problems} shader piece(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shader pieces are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
