# plotter

A pen plotter drawing the clip, as an FFGL **effect** (`PL01`, shown as
`SW Plotter`) for Resolume Arena/Avenue. C++17 + GLSL 4.10, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the planner, the machine, the ink renderer,
the sheet, or any check's tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it
  writes into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/pltest --out /tmp/f.png --size 1920x1080 --frames 720`
  (`--verbose` lists the job's strokes; `--motion` drifts the card's square)
- Set anything by name: `--set "Max Speed=0.8" --set "Pens=2" --set "Paper=3"`
  (0..1 for sliders, real integers for `Pens`, the element index for options,
  0/1 for booleans)
- Press an event before a frame: `--press "New Sheet@300"`
- List parameters, kinds, defaults and ranges: `./build/pltest --list`
- The exact GLSL the plugin compiles: `./build/pltest --dump-shaders DIR`
- Footage through the real plugin — **`--pipe`**, raw RGBA frames in, raw RGBA
  frames out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and
  an optional `--script` of `frame Parameter Name value` cues. A standard
  parameter ramps between cues; an option, boolean or integer **steps** (holds
  each cue until the next cue's frame); an event is pressed on the frame of a
  cue at 0.5 or more. A cue naming no parameter is refused with exit 2, a
  partial frame at the end of stdin ends the stream cleanly, a failed render
  or a closed stdout exits 1 (SIGPIPE is ignored):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/pltest --pipe --size 1920x1080 --fps 30 [--script cues.txt] | ffmpeg …`
  **A reel has to be filmed from its first frame**: the sheet, the machine's
  place in its job and the stabilise history all carry across frames.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + shaders + the no-GL
  checks + every GL check at 320x180 AND 1280x720 + the negative controls +
  `--pipe` + the sweep + a bench + the bundle + oxbow, ~3 min)
- The browser demo's shaders are still the plugin's: `python3 demo/tools/check_shaders.py`
  (in verify.sh). The demo's CPU half (`demo/plugin.js`) is a hand port; only a reader checks it.
- The shaders alone: `tools/check-shaders.sh build/pltest` (compiles with
  glslc if installed; always greps for GLSL 4.10 reserved words)
- No name over 16 characters, none twice: `./build/pltest --names`
- The planner and the machine against their closed forms (no GL): `./build/pltest --plan`
- A stroke takes L/v + v/a, or 2√(L/a): `./build/pltest --trapezoid`
- Ink along a stroke follows 1/v: `./build/pltest --ink`
- After t seconds, the strokes the profile predicts: `./build/pltest --budget`
- A coarse Step Size is a staircase: `./build/pltest --steps`
- Pen changes = distinct pens: `./build/pltest --pens`
- Ink stays, survives a resize, New Sheet clears it: `./build/pltest --persist`
- A filled square through the detect passes is one closed stroke: `./build/pltest --trace`
- Every check above FAILS on a perturbed plugin: `./build/pltest --negative`;
  one perturbation by hand: `./build/pltest --perturb BITS --ink` (bits in `Machine.h`)
- Every GL check takes `--size WxH`; CI runs them at 320x180
- CI's renderer, on this Mac: `PLTEST_RENDERER=software ./build/pltest --persist --size 320x180`
  (Apple's software rasteriser; `verify.sh` runs every GL check on it)
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/pltest --bench` (720p, 1080p); `--bench-4k` adds 4K, once, by hand
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Plotter.bundle`

## Notes
- **Nothing is drawn.** The heavy ends of a stroke, the blot at every
  pen-down, the darker corners, the staircase at a coarse step, the sheet as a
  collage of moments: all of it falls out of a trapezoidal motion profile and
  a renderer that deposits ink per unit *time*. If you are tempted to draw
  one of them, the chain is wrong somewhere and that is the bug.
- **`1/v` is never computed.** Each machine interval deposits `InkRate × dt`
  spread over the distance the carriage covered in it (galvo's renderer,
  turned from light into absorbance). Do not "simplify" it.
- **The paper is RGBA32F**, not 16F: the half-float blend truncates every add,
  and on a buffer that only ever accumulates that bias is systematic (0.8%
  low on a long stroke, 2.9% on a short one — `--trapezoid` measured it).
- **The machine advances by real elapsed time from SetTime, frame-relative,
  in double.** Resolume's clock is ~500 million ms into a session; no float
  ever sees an absolute time, and no shader sees a time at all.
- **The paper survives a resize** by being resampled into the new buffer
  (`PassBuffer::Swap`); `--persist` checks it texel by texel.
- **The carriage parks at the carousel** (just off the top-left corner) and
  starts a session holding no pen, so the first stroke costs one 0.35 s swap
  and one travel. Starting at the origin cost a 2 s trip first, and six
  seconds of a demo clip drew two strokes.
- **The tracer is galvo's**, copied with attribution, not rewritten. It is
  asked for work only when the machine has none (or, in Chase, whenever a
  stroke ends): one readback per job, not per frame.
- **Parameter names must be unique and ≤ 16 characters** — `--names` checks.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can
  widen it, so every slider is 0..1 and `Controls.cpp` holds the units.
  Options are mapped by index; `Pens` is a real integer.
- `sample`, `half`, `layout`, `filter`, `input`, `output`, `common`, `active`,
  `patch`, `flat`, `packed` are GLSL reserved words. `check-shaders.sh` greps.
- `far` and `near` are MSVC `windef.h` macros; galvo's `far` became `farIndex`.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no
  host can instantiate the plugin at all.
- `plotter_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `PL01`, display name `SW Plotter`.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline on
  macOS against the real plugin class, plus an `oxbow` load. No Arena, no
  Windows, no GPU other than this Mac's.
- Seen on Resolume's own demo clips only through `--pipe`, as stills.
- No OpenFX port, no browser demo, no factory presets, no user guide.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies with
  `guide=""`; register the project and re-run the syncs before a release.

## Browser demo

`demo/` is the page at **plotter-demo.stoatworks-labs.com**, deployed from
`wrangler.toml` (a Worker route over a proxied DNS record, not a custom domain)
with `cf-run npx wrangler deploy` or by any push to main — no build step; what
is committed is what is served. `demo/vendor/` is copied in by
`~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh plotter`
and is not a place to edit. Verify a deploy **by content, never by status
code**: `curl -s 'https://plotter-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/plotter/plotter.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\plotter\logs\plotter.YYYY-MM-DD.log   (Windows)
