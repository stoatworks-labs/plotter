#!/usr/bin/env python3
"""No control is silently dead.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. A
parameter the planner never reads is quieter still. So a slider can be wired
to nothing while the plugin compiles, links, loads and renders perfectly.

This renders each parameter at several positions and checks the picture
actually changed.

------------------------------------------------------------------ the traps

**A plotter is slow, so most controls need seconds, not frames.** The default
render here is 150 frames -- two and a half seconds -- which is a pen change, a
travel and the first stroke or two. A control that only shows in the third
stroke needs its own Frames. Flow, Pen Settle and Step Size all read as dead
at 120 frames on the first cut of the defaults, because nothing had been inked
yet: the sweep is also a check on the defaults' first seconds.

**Pens and Palette need a coloured stroke on the sheet.** With Optimise on the
black pen goes first (the white dashes), and two seconds is not enough to reach
the red square; with Optimise off the tracer's order puts the green bar first,
so the pen count and the palette both show in the first stroke.

**Chase does nothing on a still picture**: re-tracing the same frame at each
stroke's end plans the same job. It is swept on the moving card (`--motion`).

**Auto Sheet is a time in seconds, geometric from 1 to 300**, so the sweep's
usual thirds are 0 (never), 17 s and 300 s -- none of which fires inside a
two-second render. It is given its own values.

**New Sheet is an event.** Swept by pressing it half way through against not
pressing it at all.

**Never sweep the About block.** Those are buttons that open a web browser.

    python3 tools/sweep.py [--binary build/pltest] [--size WxH] [--jobs N]

Exit code 1 means something is dead.
"""

import argparse
import concurrent.futures
import hashlib
import pathlib
import subprocess
import sys
import tempfile

# What else has to be true for a parameter to have any effect at all, and any
# render settings it needs. "Frames", "Motion" and "Values" are not plugin
# parameters: Frames is how long to run, Motion moves the card, Values replaces
# the positions swept.
CONTEXT = {
    # The first stroke has to be a coloured one: see the docstring.
    "Pens": ["Optimise=0"],
    "Palette": ["Optimise=0"],
    # A corner's slowdown shows once the pen has been round one.
    "Corner Angle": ["Frames=240"],
    # Chase re-plans at each stroke's end; only a changing picture changes the plan.
    "Chase": ["Motion=1", "Frames=300"],
    # 0 is never; 0.05 is about 1.3 s, inside the render.
    "Auto Sheet": ["Values=0,0.05"],
    # Travel is drawn from the first move, faintly.
    "Show Travel": ["Frames=45"],
}

# Positions to try, as a fraction of the parameter's declared range. Three
# rather than two: a control that is a no-op at both ends but not in the
# middle is rare, but it costs one render to stop worrying about it.
FRACTIONS = [0.0, 0.5, 1.0]

# Parameters with no scalar value worth sweeping.
SKIP = {
    "About": "a display-only text line",
    "User guide": "a button that opens a web browser",
    "Project page": "a button that opens a web browser",
    "Source on GitHub": "a button that opens a web browser",
    "Support the work": "a button that opens a web browser",
}


def parse_list(binary):
    """Every parameter as (name, type, default, min, max)."""
    listing = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        raise RuntimeError("could not list parameters: " + listing.stderr.strip())

    rows = []
    for line in listing.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) < 5:
            continue
        # The name may contain spaces, so take the fixed columns off the end.
        low, high = float(parts[-2]), float(parts[-1])
        default, kind = float(parts[-3]), parts[-4]
        name = " ".join(parts[1:-4])
        rows.append((name, kind, default, low, high))
    return rows


def render(binary, out, settings, size, frames, motion, presses):
    command = [binary, "--out", str(out), "--size", size, "--frames", str(frames)]
    if motion:
        command.append("--motion")
    for setting in settings:
        command += ["--set", setting]
    for p in presses:
        command += ["--press", p]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("render failed: " + result.stderr.strip())
    return hashlib.sha256(out.read_bytes()).hexdigest()


def sweep_one(binary, size, row, index):
    name, kind, _default, low, high = row
    context = list(CONTEXT.get(name, []))

    frames, motion, values = 150, False, None
    for entry in list(context):
        if entry.startswith("Frames="):
            frames = int(entry.split("=", 1)[1])
            context.remove(entry)
        elif entry.startswith("Motion="):
            motion = entry.split("=", 1)[1] == "1"
            context.remove(entry)
        elif entry.startswith("Values="):
            values = [float(v) for v in entry.split("=", 1)[1].split(",")]
            context.remove(entry)

    digests = set()
    with tempfile.TemporaryDirectory() as directory:
        out = pathlib.Path(directory) / f"sweep{index}.png"
        if kind == "event":
            # Pressed half way through, against never pressed.
            digests.add(render(binary, out, context, size, frames, motion, []))
            digests.add(render(binary, out, context, size, frames, motion, [f"{name}@{frames // 2}"]))
            return name, len(digests) > 1

        # An integer or option parameter is set in its own units; a standard
        # one is 0..1 and its range says so anyway.
        if values is None:
            values = [low + (high - low) * f for f in FRACTIONS]
        if kind in ("integer", "option", "boolean"):
            values = sorted({round(v) for v in values})
        for value in values:
            digests.add(render(binary, out, context + [f"{name}={value}"], size, frames, motion, []))
    return name, len(digests) > 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/pltest")
    parser.add_argument("--size", default="480x270")
    parser.add_argument("--jobs", type=int, default=4)
    arguments = parser.parse_args()

    binary = pathlib.Path(arguments.binary)
    if not binary.exists():
        print(f"no {binary} -- build with -DPLOTTER_BUILD_TOOLS=ON first")
        return 2

    rows = [r for r in parse_list(str(binary)) if r[0] not in SKIP and r[1] != "text"]
    if not rows:
        print("no parameters found")
        return 2

    dead, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        futures = {pool.submit(sweep_one, str(binary), arguments.size, row, i): row[0]
                   for i, row in enumerate(rows)}
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                name, alive = future.result()
            except RuntimeError as error:
                print(f"  {'ERR':4}  {name}: {error}")
                failed.append(name)
                continue
            print(f"  {'ok' if alive else 'DEAD':4}  {name}")
            if not alive:
                dead.append(name)

    print()
    if failed:
        print(f"{len(failed)} parameter(s) could not be rendered: {', '.join(failed)}")
        return 1
    if dead:
        print(f"{len(dead)} parameter(s) changed nothing: {', '.join(sorted(dead))}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    print(f"all {len(rows)} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
