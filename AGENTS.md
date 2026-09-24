# AGENTS.md — Plotter

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the
short command reference; this is the *why*. Read "What is actually verified"
before you tell anybody this works.

---

## What the plugin is

A pen plotter drawing the clip, as an FFGL 2.1 effect (`PL01`, shown as
`SW Plotter`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT, home
`github.com/stoatworks-labs/plotter`, released at v0.1.0 on 2026-09-24 with a
user guide, a browser demo and a project video.

Built 2026-09-24 in one session from the fleet's templates and
`specs/SPEC-plotter.md` with `BRIEF.md` and `BRIEF-ADDENDUM.md`: **galvo** for
the tracer (copied, attributed), the detect chain, the segment renderer and
the harness shape; **toolpath** for `--pipe`, `--dump-shaders`, the negative
controls and the "would this hold" table; **tinsel** for the fleet's trap list;
**afterglow** for the idea of state that must outlive a frame; **toner** and
**slope** for the `--pipe` contract in `verify.sh`.

---

## The one idea

**galvo is a laser: a fast mirror redrawing the whole path every frame. A pen
plotter is the opposite machine. It is slow, and the ink stays.**

| the machine | what comes out |
| --- | --- |
| the pen takes the frame's path when it asks for work and draws at its own pace | **fast content never gets finished**; the sheet is a collage of the moments each stroke was drawn |
| ink is deposited per unit of TIME, spread over the distance covered | **the line is heavy where the pen is slow**: the ends of every stroke, every corner |
| the pen rests on the paper for Pen Settle at each end of a stroke | **a blot at every pen-down** |
| two stepper motors on a grid of Step Size | **staircases** on shallow diagonals, k steps by one for a slope of 1/k |
| a carousel of Pens off the corner of the sheet | **a pen change is a trip**, and the driver sorts strokes by pen to make fewer of them |

The pipeline:

1. **Detect** (GPU, galvo's): copy with a mip chain; Sobel on luma-or-alpha
   at the 320-wide trace resolution, writing the gradient AND the clip's
   colour read at that mip level; the asymmetric temporal filter.
2. **Readback** (only when work is asked for): one RGBA byte read of the
   trace buffer — the stabilised gradient and the colour together.
   Thresholded on the CPU.
3. **Trace** (CPU, galvo's `Tracer` verbatim): thin, walk, simplify.
4. **Plan** (CPU, `Planner`): contours to strokes in paper units with the
   colour along them and the nearest pen; Optimise groups by pen and tours
   each group nearest-start-first; then the blocks — travel, settle, draw
   segments under a trapezoid with junction speeds, settle, and a trip to the
   carousel at every pen change.
5. **Move** (CPU, `Machine`): the place in the block list, in double,
   advanced by the frame's real elapsed time; positions from the closed form,
   quantised to the step grid; samples for the renderer.
6. **Ink** (GPU, `render/Ink`): galvo's energy-conserving segment renderer,
   depositing `InkRate × dt` of absorbance per interval into an RGBA32F paper
   that is never decayed and is resampled on a resize.
7. **Composite**: paper, Beer–Lambert transmission through the ink, the
   travel line, the carriage, Mix. The sheet is opaque whatever the clip's
   alpha (Resolume's demo clips carry alpha).

### What falls out, and what does not

- **The heavy ends, the blots, the darker corners, the staircase** are not
  drawn: `--ink`, `--trapezoid`, `--steps` measure them out of the picture.
- **The junction speed is a straight line in the turn**, cruise at 0° to zero
  at Corner Angle. A motion controller limits a junction by centripetal jerk,
  which is monotone in the turn; this is that curve's simplest shape. The spec's
  "stop at corners sharper than a stated angle" is its endpoint.
- **At a coarse Step Size the line is beaded, not continuous.** The pen is
  modelled as *sitting* on a grid point until the next step and jumping in one
  interval, so the ink is dots at grid points joined by lighter jumps. A real
  pen drags between steps. At the default pitch (0.001 heights, one pixel at
  1080p) the beads are under a nib apart and invisible; at the coarse end the
  staircase shows as dotted runs. A drag model is an open question below.
- **The tracer is a tracer, not a vectoriser** (galvo's honest limit). On
  footage it finds a mess of short strokes, and the plotter draws them: a
  scribble that grows a few strokes a second. That is the look, not a bug.
- **Not modelled:** pen pressure, ink bleed into the paper, the pen drying,
  an acceleration limit on the travel that differs from the draw, the carriage
  mass (the profile is a pure trapezoid).

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Controls.{h,cpp}` | What a 0..1 slider means, in paper heights and seconds; the palettes; the nearest pen; the constants. |
| `source/Tracer.{h,cpp}` | **galvo's**, copied verbatim (namespace, and `far` → `farIndex` for MSVC). No GL. |
| `source/Planner.{h,cpp}` | Strokes, pen assignment, ordering, the trapezoid blocks with junction passes, `RestToRestSeconds`, `PositionInBlock`. No GL. |
| `source/Machine.{h,cpp}` | The place in the job, in double; sub-sampling; quantisation; the samples; the counters; the `Perturb` bits. No GL. |
| `source/Shaders.{h,cpp}` | copy, edge, stabilise, ink (assembled), resample, composite. |
| `source/render/Ink.{h,cpp}` | The paper and the deposit: galvo's `Beam` turned into absorbance, one RGBA32F buffer, resampled on resize. |
| `source/PassBuffer.*` | galvo's FFGLFBO with the leak fixed, plus `Swap`. |
| `source/Plotter.{h,cpp}` | The plugin: parameters, the clock, when to ask for work, the sheet, the passes, the test hooks. |
| `source/Diag.*` | A log file, for the shader that will not compile. |
| `tools/pltest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/check-shaders.sh` | glslc over the dumped shaders, and a reserved-word grep. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |
| `demo/` | The browser demo: `plugin.js` holds every shader piece verbatim and a PORT of the CPU half; `tools/check_shaders.py` keeps the pieces identical; `vendor/` is the shared kit from `stoatworks-backend/resolume-demo` (never edit it, re-run `sync.sh`). Served by this repo's own Worker at `plotter-demo.stoatworks-labs.com` through a DNS record + route. |

---

## Traps

Roughly in the order they will bite.

### ☠️ A half-float paper loses ink on every add, and it never gives it back

The paper was RGBA16F, as galvo's accumulation buffer is (galvo measured 16F
0.33% low and shipped it for the speed). Here `--trapezoid` read the pen-down
time off the sheet **0.8% low on a long stroke and 2.9% low on a short one**.
The GPU's blend rounds each addition into a half float, and on a buffer that
only ever accumulates — no decay, thousands of small deposits into texels
already holding a few units — the rounding is a bias, not noise: worst where
the ink is thickest, which is the blots this plugin is about. RGBA32F reads
−0.007%. The cost is 133 MB more at 4K and nothing measurable in `--bench`.

### ☠️ The carriage's first trip is the first seconds of every clip

Started at the origin, the machine's first job began with a 1.8 s travel to
the carousel, a 0.5 s swap, and a travel back: six seconds of a Resolume demo
clip had drawn two short strokes, and the sweep reported Flow, Pen Settle and
Step Size dead because nothing had been inked in its two seconds. The carriage
now **parks at the carousel** (`Machine::Reset`), the swap is 0.35 s, rapids are
3× the draw speed, and the defaults are 0.6 heights/s at 4 heights/s². The
sweep's 150-frame render is deliberately also a check on the defaults' first
seconds.

### ☠️ galvo's default threshold finds nothing on colour

At galvo's 0.25 the test card's red square, blue ring and green bar — luma 0.3
against a grey of 0.15 — were not edges at all, and the job was the
transparent hole and three white dashes. galvo's card is brighter and galvo is
for artwork; footage is low in luma contrast. The default is 0.14 and the card's
shapes are brighter too.

### ☠️ RGB distance sends white to the blue pen

Nearest-pen by Euclidean RGB: white is far from every pen, and nearest, absurdly,
the blue one; a mid grey went to the green. Most edges in footage are bright
and unsaturated. `NearestPen` sends anything with chroma under 0.3 of its
value, or a value under 0.25, to the black pen first, and only then measures
distance.

### ☠️ A circle simplified to nine points is an octagon of full stops

With a binary corner rule (stop above 30°), galvo's tracer turning the hole
into a 9-gon with 45° turns made the pen stop eight times round a circle,
each stop a blot. The junction speed is now continuous in the turn (above),
and the default Corner Angle is 45°.

### ☠️ The negative control must reach the planted job

`--budget` plants its bars with `SetJobForTest(…, once)`, and the first cut of
the "retrace every frame" perturbation was gated off for planted jobs, so the
negative control passed — the check could not fail. The perturbation now
re-plans whatever job the plugin has, planted or traced, from its first
stroke every frame.

### ☠️ Risers stand on grid points, not at run ends

The first `--steps` read the centroid at 0.3 and 0.7 of each run, assuming the
riser sat at the run's end. It sits on the grid point nearest the end — up to
half a pitch away — and its tail moved the centroid by 0.05 p, failing 19 of
38 columns at 720p. Columns are now read half way between grid points, one
pitch either side of the run's centre, where the nearest riser is 3σ away at
σ = p/6 and the unquantised line is p/6 off the level.

### ☠️ A finished bar reads longer by the settle's blot

`--budget` read finished bars 0.016 too long at a tolerance of 0.0094: the
settle at each end is a dot four times the cruise peak, and the 50% crossing
runs past the end to where the dot has fallen to half cruise, a radius
σ√(2 ln(blot/half-cruise)) = 2.04σ. It is now in the prediction, not the
tolerance.

### ☠️ The driver starts from the carousel, so bar 0 is not first

The harness's schedule assumed the bars in order; the plugin's nearest-start
tour from the carousel, off the top-left corner, takes the top bar first and
works down. The harness now replicates the tour rather than assuming it.

### ☠️ A mutation the checks cannot see is not a failed mutation test

`across − pedestal` → `across + pedestal` in the ink fragment shader changed
nothing any check could measure: the pedestal is e^-10.1 of the peak and the
flip moves the total ink by 2.8e-4, under every tolerance. That is the
tolerance being right, not the harness being blind — but it is not a mutation
test. `-0.5` → `-0.6` in the same Gaussian is (below).

### ☠️ A reader that leaves kills --pipe with SIGPIPE

The fleet's contract says a closed stdout exits 1. With a real pipe the next
write raises SIGPIPE and the process dies with 141 and no word on stderr.
`--pipe` ignores SIGPIPE, so the write fails with EPIPE and the loop says so;
`verify.sh` checks `| head -c 1`.

### ☠️ The step-cue pipe test must be able to fail

Keys White@0 → Alpha@2 cannot tell a step from a ramp at frame 1: the ramp
lands on Ghost, which is opaque like White. Alpha@0 → White@2 can: stepped,
frame 1 is Alpha and transparent where uninked; ramped, it is Ghost. And the
carriage has to be hidden, because at 36 rows its ring's 1.5 px feather reaches
the corner pixel the test reads (alpha 164 on a "blank" sheet).

### ☠️ The first job after a cut wore the previous clip's colours

Filming found it: Resolume's three coloured rings, cut in after a sphere of
orange shingles, were drawn in red, and after a grey mask in black. The
colour along a stroke is read out of the stabilised trace buffer, and the
temporal filter's slow release carries the previous picture's colour as well
as its gradient, so a job traced on the frame of a tear-off was the old
picture's edges in the old picture's colours. A tear-off (New Sheet, or the
Auto Sheet timer) is now decided before the stabilise pass and drops the
history, so the new sheet's first job is traced from the picture now. A hard
cut without a tear-off still fades the old edges out over the release, which
is the filter doing its job.

### ☠️ Palette Source is a dark pen on a dark ground

Also found filming. The stroke's colour is sampled where the edge is, at the
trace mip level, so on a bright shape against black it is half background:
Trinity's rings drew near-black under Source, and the Technical pens sent the
same sample to the black pen by the grey rule. A survey of every bundled demo
clip with eight pens found colour only where the edges lie inside colour
(Metalive's shingles, the dancers' skin, Cyberspace's red lines). The guide
says so; nothing was changed, because the sample is honest.

### ☠️ Mutation-test only a committed tree

Both mutations were applied to a clean, committed tree and reverted with
`git checkout`; nothing uncommitted was lost with them.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured
first and put back before the composite); every `ffglex::Scoped*` clears to 0
on exit, so every allocation happens before this frame binds anything;
`FFGLFBO::Release()` leaks the colour texture (`PassBuffer::Destroy` deletes it
first); `SetParamInfo` clamps a STANDARD default into 0..1 before
`SetParamRange` can widen it; the core is an **OBJECT** library;
`SetTextParameter` must return `FF_SUCCESS` for the About block; the harness
drives a synthetic clock (`SetTime`, frame / fps, the unit declared — never
`steady_clock`, which is why resodoom's harness had to pace in real time);
`nm | grep -q` fails under pipefail when grep succeeds; an option's range reads
back 0..1 whatever its element count; Resolume's clock overflows a float
(the frame delta is taken in double and no shader sees a time); a buffer
holding state across frames must survive a resize; `max( luma * a, a )` is 1
for every opaque pixel (Luma or Alpha is `a (0.35 + 0.65 luma)`); `packed`,
`sample`, `half`, `layout`, `filter`, `input`, `output`, `common`, `active`,
`patch`, `flat` are GLSL reserved words (`check-shaders.sh` greps for them,
because Mesa refuses `packed` and Apple does not); MSVC has no `M_PI` and
`far`/`near` are its macros (`kPi`, `farIndex`); Resolume's demo clips are
DXV with alpha, so the sheet's alpha is 1 deliberately.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh`. H is the raster's height; σ is the nib's
sigma in paper units (1 = H).

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--trapezoid`, ink route | the pen-down time as (absorbance summed over the sheet) / (InkRate × weight × H²): the renderer's conservation makes the sum InkRate × T × H² | **1%**: the tanh CDF is good to 3e-4; the 4.5σ cut and pedestal 1.4e-4; the Gaussian's pixel-sum aliasing 2 exp(−2π²σ_px²), 7e-5 at σ = 0.72 px (180 lines); 32F accumulation under 1e-4. Measured −0.007% (720p), −0.08% (180p) | none once σ_px ≥ 0.7 (the aliasing term); at 180 lines σ = 0.004 is 0.72 px, the floor |
| `--trapezoid`, frame route | first frame with ink to last frame it grew | **2/fps**: one frame of quantisation at each end | none |
| `--ink` | column integrals of absorbance at s = v²/8a (speed v/2), the middle, and L − s, as a ratio | **2%**: the nib smooths the 1/√s profile by (σ²/2)(3/4s²) = 0.3% at σ 0.004, s 0.045; the interval's uniform spread is second order in a·dt/v (the machine holds it under 1% of v per interval); 32F under 1e-4. The across profile is identical at every column and cancels. Measured +0.3% | the line is on a row centre and the columns are pixel centres at any raster; s is recomputed from the column's centre |
| `--budget` | per bar, at checkpoints: finished (extent = L + 2 blot radii), current (extent = s(t) + one), untouched (peak < 5% cruise) | **σ + 1 px**: the 50% crossing of a profile no steeper than the nib, read between two pixel columns; the blot's radius σ√(2 ln(blot/half-cruise)) is in the prediction | the bars are on row centres and start at a column centre; the tolerance's pixel term scales |
| `--steps`, levels | the ink-weighted centroid row at columns half way between grid points, one pitch either side of each run's centre | **0.1 px + 0.03 p**: a Gaussian of σ ≥ 0.7 px sampled at rows has its centroid to 1e-3 px; the nearest riser is half a pitch = 3σ away, tail e^-4.5. The unquantised line is p/6 off there (0.42 px at 180p, 1.75 at 720p), which the check asserts exceeds the tolerance | σ = p/6 is 0.75 px at 180 lines, the floor |
| `--steps`, runs | the x at which the centroid crosses half way between levels | **one pitch** (the spec's); measured 0.33 px at 720p, 0.05 at 180p | none |
| `--pens` | trips to the carousel, counted by the machine; each stroke's g/r absorbance ratio at its middle against (1 − ink_g)/(1 − ink_r) | **exact** counts; **2%** on the ratio (32F on two channels; measured 0.00%) | none |
| `--persist` | the sheet after 30 idle frames; every texel of a 2× sheet against the bilinear blend of its four old neighbours at quarter offsets; the total; New Sheet | **byte for byte**; **0.6% of the old maximum + 1e-5** (8-bit filter weights on some GPUs, 32F); **2%** on the ×4 total; **exactly 0** | the resize is to twice whatever raster is given |
| `--trace` | one closed black stroke's perimeter; then ink within 0.02 of the outline and none 0.06 inside or outside | **8%** (galvo's: the Sobel band and the thinning put the skeleton within a trace texel of the edge per side, plus 1.15 texels of simplification); ink inside/outside under 0.1% of the outline's | at 320 wide the trace is the picture; measured 1.3047 for 1.3333 at both |
| `--plan` (no GL) | block durations against L/v + v/a and 2√(L/a); v/a saved by a straight junction and v/4a cost by a half-speed one; reachability; pen changes; nearest pen; the machine's pen-down time, sample sum and end position over frames of three lengths; samples on the grid | **1e-6 relative** (the strokes' endpoints are floats); **1e-9** on reachability; **1e-9 s** on the machine's clock; **1e-4 p** on the grid | no raster |

Deliberately NOT relied on: exact cancellation (every ratio has a stated
tolerance), `pow(1,1)`, the order fragments are blended in (additive blending
in 32F is associative to 1e-7), implicit derivatives (none in any shader).

What might still differ on another rasteriser: the bilinear filter's sub-texel
weights in the resample (the `--persist` tolerance allows 8 bits); `exp` and
`tanh` within their stated ULPs; the 32F blend, which GL leaves to the
implementation (a half-float blend path would show up as `--trapezoid`
reading low, as it did here). Every GL check also runs on Apple's software
rasteriser (`PLTEST_RENDERER=software`, the renderer GitHub's macOS runners
fall back to, which is not repeatable at the last bit) at 320×180 in
`verify.sh`: all eight pass there, `--persist`'s byte-for-byte included,
because an idle machine deposits nothing and the paper is only read.

---

## Negative controls and the mutation

`pltest --negative` runs six, and `--perturb BITS` runs any check verbosely
against one. Each perturbs the *plugin* — a `Perturb` bit the shipped plugin
carries at zero — never the harness's expectation. At 320×180 and 1280×720:

| perturbation | what fails |
| --- | --- |
| infinite acceleration (`kPerturbInfiniteAccel`) | `--trapezoid`: the stroke takes L/v, 23% short of L/v + v/a; `--ink`: both ratios 1.00 against 2.01 |
| a job every frame, from its first stroke (`kPerturbRetraceEachFrame`) | `--budget`: every bar untouched at every checkpoint |
| Step Size ignored (`kPerturbNoQuantise`) | `--steps`: the centroid follows the line, p/6 off every level |
| Optimise ignores the pen (`kPerturbNoPenSort`) | `--pens`: 8 trips for 4 pens |
| a resize clears the sheet (`kPerturbResizeClears`) | `--persist`: the ×2 sheet is empty |

(`kPerturbNoStopAtCorner` exists for hand use; no shipped check depends on it.)

### The mutation

One character of the shipped GLSL, on a clean committed tree (8e4a1de + the
pipe-test fix): in the ink fragment shader, the nib's across-profile
`exp( -0.5 * v * v * inv * inv )` → `-0.6`, which mis-normalises the Gaussian
by 1/√1.2. Caught by **`--trapezoid`** at both rasters: the pen-down time read
off the sheet came out **−8.81% (320×180) and −8.72% (1280×720)** against a
1% tolerance, on both the cruise and the triangle case. **Not** caught by
`--ink`, `--budget`, `--steps`, `--pens`, `--persist` or `--trace`, correctly:
every one of those reads a ratio, a position or a count that a uniform
mis-scaling of the nib cancels out of. Reverted with
`git checkout source/Shaders.cpp`; the tree was clean before and after.

A first mutation, `across - pedestal` → `across + pedestal`, was **not** caught
by any check at either raster: the pedestal is the Gaussian's value at 4.5σ,
e^-10.1 of its peak, and the flip moves the total ink by 2.8e-4 (see the trap
above). Recorded because a mutation that cannot be seen says something about
the tolerances, not about the harness.

---

## The browser demo

`demo/` is the page at **plotter-demo.stoatworks-labs.com**, built on galvo's
`beed479` (the tracer is galvo's, so its JS port of the tracer is the start).
Like galvo's it is the hard case in that suite: **this plugin is not a
shader.** Between the detect passes and the ink renderer sit `Tracer`,
`Planner`, `Machine`, `Controls` and the CPU side of `render/Ink`, and without
them the page renders blank paper. So all of them are ported into
`demo/plugin.js`, in JavaScript, function for function, and that port is the
demo's weak point: `check_shaders.py` proves the nine GLSL pieces are the
plugin's (the ink pass is assembled from three of them, here as there), and
**nothing at all proves the port is**. Change one of those files and change
the page by hand to match. The page says this in its banner and in its
disclosure; do not soften either.

What the page does that the plugin does not, each forced by WebGL2 and each
disclosed on the page:

- **The paper is RGBA32F only where the browser allows it.** WebGL2 blends
  into a 32F target only with `EXT_float_blend` and filters one linearly only
  with `OES_texture_float_linear`; without both the paper is RGBA16F, which is
  the half-float bias the trap above measured and rejected, and the line under
  the canvas says so. Without `EXT_color_buffer_float` the page refuses to
  start rather than accumulate ink into eight bits.
- **The stabilise buffers are RGBA8**, because WebGL2 will not read a float
  framebuffer back as bytes and the readback is bytes in the plugin too.
- **Pens is a dropdown** (no integer control in the kit) and **New Sheet is a
  button** inserted at its declared position (no event control in the kit).
- **The clock is the kit's**: seconds declared, so the unit vote never runs.
  Restart restarts the clip, not the sheet.
- The `Perturb` hooks, `SetJobForTest` and `ReadPaperForTest` are not ported.

Deploy: `cf-run npx wrangler deploy` from the repo root, or push to main
(`.github/workflows/deploy.yml`). The host is a Worker **route** over a
proxied `AAAA 100::` DNS record, not a custom domain: the zone hit
Cloudflare's 100-custom-domain limit on 2026-09-24. Delete that record and
the page goes dark while deploys stay green. Verify by content:
`curl -s 'https://plotter-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

## Decisions taken without asking

- **Lengths in paper heights, times in seconds.** Max Speed 0.05–2 heights/s,
  Acceleration 0.1–20 heights/s², Step Size a pitch in heights: the same
  drawing at the same pace on any composition raster.
- **Ink per unit time, with Flow as the darkness at a reference speed.**
  `InkRate = Flow × v_ref × σ × √2π`, so Flow means the same absorbance at
  every nib width and Max Speed still changes the weight, as on a real machine.
  Absorbance, Beer–Lambert: ink is subtractive, a red pen absorbs green and blue.
- **The paper is RGBA32F** (the trap above).
- **The junction speed falls linearly with the turn** to zero at Corner Angle
  (the trap above), default 45°.
- **The tracer runs once a job**, not once a frame; in Chase, once a stroke. An
  idle machine on a blank clip retries every 0.25 s, not every frame.
- **The carriage parks at the carousel**, just off the top-left corner, and a
  session starts with no pen: the first stroke of a session always pays one
  swap. `--pens` counts that pickup.
- **Optimise starts with the pen the carriage holds**, then ascending; each
  group is galvo's nearest-neighbour tour (open strokes reversible, closed ones
  rotatable). Off, the tracer's order, pens interleaved.
- **Palettes**: Technical (black + seven drawing-office colours), Black (every
  pen black: Pens then only sorts), Neon, Source (the stroke's own colour with
  its value capped at 0.7 so a white edge draws, sorted onto the Technical
  pens). Greys go to the black pen before any distance is measured.
- **Paper defaults to Ghost** — white with the clip at 22% under it — so the
  frame is never empty while the pen thinks. The sheet is opaque in every mode
  but Alpha, whatever the clip's alpha.
- **Auto Sheet defaults to 30 s.** A finished job on a still clip is re-traced
  onto the same sheet (the pen goes over its own lines), which is what a
  plotter does; the sheet is torn off on the timer or the event.
- **`New Sheet` is an event** (FF_TYPE_EVENT); a press is remembered until the
  next frame.
- **Trace Size is fixed at 320**, galvo's default; Min Length 8 and Simplify
  1.15 trace pixels are constants. The spec's Path group is Threshold, Detail,
  Chase.
- **No factory presets, no OpenFX.** The browser demo (`demo/`) came with the release.
- **Test hooks live in the shipped plugin** (`Perturb` bits, `SetJobForTest`,
  `ReadPaperForTest`), always inert.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated** by the backend's
  `sync-about.py` and `sync-attributions.py`; the user guide is `docs/USER-GUIDE.md`,
  rendered to the site and `docs/USER-GUIDE.pdf` by the website's `build_guides.py`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-24)

Every number is `tools/verify.sh` on this machine against a fresh universal
Release build, at 320×180 and 1280×720.

- **Trapezoid.** Pen-down time off the sheet 2.7998 s for 2.8000 (L 0.6, v 0.3,
  a 0.5, two 0.1 s settles) and 0.6000 for 0.6000 (L 0.02); by frames within
  one frame; −0.08% at 180p.
- **Ink.** 2.018 / 2.018 (720p), 2.020 / 2.020 (180p) against 2.0125 at both ends.
- **Budget.** Every checkpoint of a 17.68 s five-bar job at both rasters; the
  harness's closed-form schedule and the machine's agree to 0.01 s.
- **Steps.** 18 columns on their level to 0.000 px against 0.64 / 0.235 px;
  9 runs of 0.150 to 0.33 / 0.05 px.
- **Pens.** 4 trips with Optimise, 8 without; colours to 0.00%.
- **Persist.** Byte for byte; 464,456 / 32,088 resampled texels, worst 1.2e-7;
  ×4.0000; New Sheet to 0.
- **Trace.** One closed black stroke, 1.3047 for 1.3333; inked after 10 s,
  nothing inside or outside.
- **Negative controls.** All six fail their check at both rasters.
- **Mutation.** Caught by `--trapezoid` at both rasters (above).
- **No dead controls**, all 19, the four About buttons skipped.
- **Every shader compiles** through `glslc`, all eight, as the plugin
  assembles them; none uses a reserved word.
- **`--pipe`** returns exactly two frames for two and a half, refuses an
  unknown cue, steps an option cue, and exits 1 on a failed render and on a
  closed stdout.
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.plotter`, ad-hoc signs, and `oxbow` reports
  `SW Plotter` / `PL01` / `effect` and renders 120 frames through `plugMain`.
- **On Resolume's demo clips** (`Beat 001`, `Trinity_09`, `NeonRoom2_32`,
  `OrganicMotions_06`; DXV, 1280×720, RGBA), through `pltest --pipe` at
  960×540: stills at 1, 3 and 6 s show ink from the second second, the ghosted
  clip under it, an opaque frame, and the carriage. Nothing was timed there.
- **Render cost** at the defaults on the test card, 60 frames after a
  20-frame warm-up, `glFinish` both sides, on a shared GPU, one run of
  `--bench-4k`:

  | | as shipped ms/frame | traced every frame ms/frame | of which the tracer ms |
  | --- | --- | --- | --- |
  | 1280×720 | 0.19 – 0.21 | 0.73 – 1.69 | 0.59 – 1.44 |
  | 1920×1080 | 0.20 – 0.22 | 0.77 – 2.05 | 0.64 – 1.72 |
  | 3840×2160 | 0.53 | 1.15 | 0.98 |

  Ranges over two runs (`--bench-4k` by hand, then `verify.sh`'s `--bench`)
  on a GPU shared with several other builds: the shipped figure barely moves,
  the traced-every-frame one doubles, and the difference is the readback
  stall, which waits on whatever else was queued (galvo's finding). As
  shipped the tracer runs once a job — a stall of one to two milliseconds
  once every ten to thirty seconds — and the frame is the detect passes and
  the composite. The "traced every frame" column is the `kPerturbRetraceEachFrame`
  hook and is the worst case Chase can approach on a clip of very short strokes.

### Assumed, or not done

- ☠️ **Never loaded into Resolume on macOS.** Everything there was compiled,
  rendered and measured offline against the real plugin class in a headless
  CGL context, plus an `oxbow` load.
- **Windows:** On Windows, in Resolume Arena 7.27.1 (win-lab, Mesa llvmpipe, no GPU, 2026-09-24), the DLL of this source loads from Extra Effects, registers as `SW Plotter` / `PL01` / effect, all 25 host controls match the declaration, it renders, Arena's log stays clean and all 19 controls move the picture: 9 of the fleet gate's 9 checks, in two runs (before and after the tear-off fix). Software rendering says nothing about a GPU or about speed.
- **Seen on footage only through `--pipe`** (a survey of every bundled demo clip
  at the defaults, then the project video at 30 fps). The pace reads right at 30 fps
  on a render; whether it does at 60 fps in a host is a judgement nobody has made.
  Filming found that **Palette Source is a dark pen on a dark ground**: the stroke's
  colour is sampled at the edge, half background, and the Technical pens' nearest-pen
  rule sends that sample to black too (Resolume's three coloured rings drew black under
  both). A clip whose edges lie inside colour (the dancers) draws in colour.
- **The clock-unit voting** is galvo's, which has met Arena on Windows; this
  plugin has not.
- **The 1/v law inside a sample interval** is held to 1% of v by sub-sampling;
  the check measures at 2%. Nothing measures the corner slowdown's ink directly
  (`--plan` measures its timing).
- **The tracer's cost on footage** is not measured: `--bench` traces the test
  card (eight strokes). A busy frame of footage has hundreds.
- **Windows** builds in CI (MSVC, GLEW from vcpkg); see the Arena line above.
- **No OpenFX port.** The browser demo is a port to a web page; its CPU half is a
  hand port that only a reader checks (see `demo/`).
- **Nothing has been through a show.**

---

## Open questions

- **Should the pen drag between steps?** At a coarse Step Size the line is
  beaded at grid points (above). A first-order lag on the quantised position
  would join the beads into the continuous staircase a cheap plotter draws,
  at the price of a time constant to expose or fix.
- **Should a finished job wait for the picture to change?** On a still clip
  the pen re-traces the same strokes onto the same sheet until Auto Sheet
  fires. A difference threshold on the mask would let it idle instead.
- **Should Chase carry the pen's group?** Chase re-plans from the current pen
  first, so a coloured clip is drawn one pen at a time even while it moves; a
  chase that ignored the pen sort would change colour more and draw more of
  "now".
- **Is 320 the right trace width for a plotter?** A plotter's resolution is
  its step; the tracer's only sets where the path comes from. 320 makes the
  readback 230 KB and the strokes coarse on 4K; galvo exposes it and this does not.
- **Is the ordering's cost bounded on footage?** galvo's tour is O(n²) over
  contours; a busy frame of footage has hundreds. It runs once a job, so it
  has not mattered; in Chase on a very busy clip it runs once a stroke.

---

## Siblings

- **galvo** — the tracer (verbatim), the detect chain, the segment renderer, the
  harness shape, the clock voting, the About headers.
- **toolpath** — `--pipe`, `--dump-shaders` + `check-shaders.sh`, the negative
  controls, the "would this hold" table, the resize-mid-run check.
- **tinsel** — the luma-or-alpha mode, `PassBuffer`, the sweep, the trap list.
- **afterglow** — state that outlives a frame.
- **toner**, **slope** — the `--pipe` contract in `verify.sh`, SIGPIPE ignored.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
