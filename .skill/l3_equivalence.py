"""
L3: does a rebuilt mirror behave like the asset it was exported from?

L1 compares export text and L2 compares whether the result compiles. Neither of them
notices a system that builds cleanly and then simulates differently, which is the failure
an artist would actually see. This runs both systems and compares what they produce.

Method (plan-v5 R4 step 6). For each pair, spawn a component, then drive it with
`CaptureNiagaraSimCacheImmediate(bAdvanceSimulation=True, AdvanceDeltaTime=1/30)` a fixed
number of times and read each emitter's particle count per frame. Advancing by a fixed step
rather than waiting on the editor's realtime tick is what makes the result reproducible --
a wall-clock capture gives a different answer every run.

Two verdicts per pair, because the first run of this protocol produced three "failures" that
were all the same shape -- the whole series shifted by one frame -- and a shift is not a
behavioural difference:

  exact   every frame equal, emitter by emitter
  phased  equal after aligning each series on its first non-zero frame, plus totals and peak

A pair that is `phased` but not `exact` is reported separately rather than as a pass: it means
the two agree on what they simulate and disagree on when, which is worth a look but is not the
same defect as a mirror that spawns nothing.

Run inside the editor -- a commandlet's null RHI does not simulate at all. Load the harness, then
step it one pair per call (see the note above l3_begin for why the loop cannot be internal):

    B=.claude/skills/unreal-bridge/scripts/bridge.py
    python $B --no-preflight exec-file "Plugins/DreamFX/.skill/l3_equivalence.py"
    for i in $(seq 0 44); do
      python $B --no-preflight exec "l3_side_a($i)"
      python $B --no-preflight exec "l3_side_b($i)"
      python $B --no-preflight exec "l3_side_c($i)"
    done
    python $B --no-preflight exec "l3_report()"

One system per call, never both sides of a pair in one. Measuring a pair in a single call reports
identical systems as different -- that is what the note above l3_side_a is about.

The first system measured after the editor starts reads all zeros. Step one pair and discard it,
then l3_begin() to reset, before trusting anything.

GPU emitters report zero here even in the editor -- there is no readback of their particle buffer on
this path. That is symmetric between the two sides of every pair, so it costs coverage, not
correctness: a GPU-only difference is invisible to this check rather than misreported by it.

Writes a markdown table to Saved/DreamFX/l3-report.md.
"""

import os
import unreal

# 45, not 24: an emitter behind a LoopDelay of 1.0s first spawns at frame 30, and Teleport's
# NE_C -- the round's motivating case -- is exactly that. A window the effect never starts in
# judges every channel equal on both sides.
FRAMES = 45
DELTA = 1.0 / 30.0

# A mirror is <Mount>/Decompiled/<the original's own path below its mount point>, so the pair is
# recovered by deleting that one segment. Scanning /Game alone would have missed 16 of 45 -- the
# HairStrands, VRM4U, Water and DreamFX packs all mirror inside their own mount.
MIRROR_SEGMENT = "/Decompiled/"

NIAGARA_SYSTEM_CLASS = unreal.TopLevelAssetPath("/Script/Niagara", "NiagaraSystem")


def _iter_mirror_pairs():
    """Yields (original_path, mirror_path) for every mirror that has a source asset."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.wait_for_completion()

    # By class rather than by path: the class index is the cheap way to reach every mount at once,
    # where get_assets_by_path would have to be called per mount point and told each one's name.
    for asset in registry.get_assets_by_class(NIAGARA_SYSTEM_CLASS, search_sub_classes=False):
        mirror_path = str(asset.package_name)
        index = mirror_path.find(MIRROR_SEGMENT)
        if index == -1:
            continue

        original_path = mirror_path[:index] + "/" + mirror_path[index + len(MIRROR_SEGMENT):]

        # DoesAssetExist rather than the registry's GetAssetByObjectPath: that one took an FName in
        # UE4 and an FSoftObjectPath now, so which overload Python binds to depends on the version.
        if unreal.EditorAssetLibrary.does_asset_exist(original_path):
            yield original_path, mirror_path


# The channels beyond the particle count, and how to read each. Chosen for what the round of
# 2026-08-11 proved invisible: NE_C's mirror agreed with its original frame-for-frame on COUNT
# while carrying no Color, Scale or Position at all -- the user's eyes were the first detector.
# Mean plus per-component min/max is enough to catch both "attribute missing" (reads empty while
# particles exist) and "attribute wrong" (colour scaled differently, sizes off), without storing
# every particle.
CHANNELS = (
    ("Position", "position"),
    ("Color", "color"),
    ("Scale", "vector"),
    ("SpriteSize", "vector2"),
)


def _components(value):
    """The numeric components of whatever a Read*Attribute call yields."""
    for axis in ("x", "y", "z", "w", "r", "g", "b", "a"):
        if not hasattr(value, axis):
            continue
        return [getattr(value, axis) for axis in
                [a for a in ("x", "y", "z", "w", "r", "g", "b", "a") if hasattr(value, a)]]
    return [float(value)]


def _aggregate(values):
    """[mean..., min..., max...] over each component, or None for an empty read."""
    if not values:
        return None
    rows = [_components(v) for v in values]
    width = len(rows[0])
    cols = [[row[i] for row in rows] for i in range(width)]
    return ([sum(c) / len(c) for c in cols] + [min(c) for c in cols] + [max(c) for c in cols])


def _counts_per_frame(system_path):
    """
    Per-emitter series, as {emitter: {"n": [n0, ...], "<Channel>": [aggregate0, ...]}}.

    A channel frame is an [mean+min+max] list, None while the emitter has no particles that frame,
    or the string "absent" when particles exist and the attribute reads empty -- the distinction is
    the whole point, because "absent" is exactly the state every text-level check missed on NE_C.

    Returns None when the system will not load or will not capture, so a broken asset is
    reported as such instead of silently comparing as empty.
    """
    system = unreal.load_asset(system_path)
    if system is None:
        return None

    # The supported spawn path, rather than constructing a component and registering it by hand:
    # a hand-registered component is not guaranteed to have a system instance, and a capture of
    # one silently reports zero particles -- which is indistinguishable from a real difference.
    # UnrealEditorSubsystem rather than EditorLevelLibrary: the library is the UE4-era scripting
    # shim, and GetEditorWorld lives on the subsystem now.
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    component = unreal.NiagaraFunctionLibrary.spawn_system_at_location(
        world, system, unreal.Vector(0, 0, 0), auto_destroy=False, auto_activate=True)
    if component is None:
        return None

    # The cache has to exist first. CaptureCurrentFrameImmediate opens with
    # `if (SimCache != nullptr && NiagaraComponent != nullptr)` and otherwise does nothing at all,
    # so passing None is a silent no-op that reports every system as having no particles.
    cache = unreal.NiagaraSimCacheFunctionLibrary.create_niagara_sim_cache(world)

    series = {}
    try:
        for _ in range(FRAMES):
            # Python collapses the bool return and the out-parameter into one value, so this is the
            # cache back again (or None) and not a success flag -- the frame count is what says
            # whether anything was written. Each call is a fresh BeginWrite/EndWrite pair, so the
            # frame just captured is always index 0.
            captured = unreal.NiagaraSimCacheFunctionLibrary.capture_niagara_sim_cache_immediate(
                cache, unreal.NiagaraSimCacheCreateParameters(), component,
                advance_simulation=True, advance_delta_time=DELTA)
            if captured is None or captured.get_num_frames() == 0:
                continue
            for emitter in captured.get_emitter_names():
                name = str(emitter)
                entry = series.setdefault(name, {"n": []})

                positions = captured.read_position_attribute(
                    attribute_name="Position", emitter_name=emitter, frame_index=0)
                count = len(positions)
                entry["n"].append(count)

                for channel, kind in CHANNELS:
                    if kind == "position":
                        values = positions
                    elif kind == "color":
                        values = captured.read_color_attribute(
                            attribute_name=channel, emitter_name=emitter, frame_index=0)
                    elif kind == "vector":
                        values = captured.read_vector_attribute(
                            attribute_name=channel, emitter_name=emitter, frame_index=0)
                    else:
                        values = captured.read_vector2_attribute(
                            attribute_name=channel, emitter_name=emitter, frame_index=0)

                    if count == 0:
                        frame_value = None
                    elif len(values) == 0:
                        frame_value = "absent"
                    else:
                        frame_value = _aggregate(values)
                    entry.setdefault(channel, []).append(frame_value)
    finally:
        component.deactivate()
        # DestroyComponent takes the caller: "may not be used to destroy a component that is owned
        # by an actor unless the owning actor is calling the function". Spawning into the editor
        # world attaches to the level's WorldSettings, so that is the owner to name.
        component.destroy_component(component.get_owner())

    return series


def _first_active(counts):
    """Index of the first non-zero frame, or None for an emitter that never spawns."""
    for index, value in enumerate(counts):
        if value != 0:
            return index
    return None


def _channel_mismatch(name, left_entry, right_entry, shift_left, shift_right, values=True):
    """The first channel-level disagreement between two emitters' series, or None.

    values=False compares only ABSENCE -- particles present while the attribute reads empty.
    Absence is structural (the compiled scripts either produce the attribute or they do not), so
    it is judgeable even on a system whose values do not replay: NE_C's fossil mirror is exactly
    an absence, and it must not hide behind the system's own randomness.
    """
    for channel, _ in CHANNELS:
        a = left_entry.get(channel, [])[shift_left:]
        b = right_entry.get(channel, [])[shift_right:]
        span = min(len(a), len(b))
        for frame in range(span):
            if (a[frame] == "absent") != (b[frame] == "absent"):
                side = "mirror" if a[frame] != "absent" else "original"
                return f"'{name}' {channel} is absent on the {side} side"
            if values and a[frame] != b[frame]:
                return f"'{name}' {channel} differs (frame {frame} after alignment)"
    return None


def _absence_mismatch(left, right):
    """Absence-only comparison across all emitters, for pairs whose values are undecidable.

    An emitter empty on one side counts as absence too: the count is read through Position, so a
    compiled-out Position makes the whole emitter read as zero particles -- which is exactly how
    NE_C's fossil mirror presents, and it is structural, not random.
    """
    if left is None or right is None or set(left) != set(right):
        return None
    for name in left:
        start_a = _first_active(left[name]["n"])
        start_b = _first_active(right[name]["n"])
        if (start_a is None) != (start_b is None):
            side = "mirror" if start_a is not None else "original"
            return f"'{name}' is empty on the {side} side"
        mismatch = _channel_mismatch(name, left[name], right[name],
                                     start_a or 0, start_b or 0, values=False)
        if mismatch is not None:
            return mismatch
    return None


def _compare(left, right):
    """Returns 'exact', 'phased', or a short reason the two disagree."""
    if left is None or right is None:
        return "asset would not capture"
    if set(left) != set(right):
        missing = sorted(set(left) ^ set(right))
        return f"emitter set differs ({', '.join(missing[:3])})"

    if all(left[name] == right[name] for name in left):
        return "exact"

    for name in left:
        counts_a, counts_b = left[name]["n"], right[name]["n"]
        start_a, start_b = _first_active(counts_a), _first_active(counts_b)
        if (start_a is None) != (start_b is None):
            return f"'{name}' is empty in one side"
        shift_a, shift_b = start_a or 0, start_b or 0

        a, b = counts_a[shift_a:], counts_b[shift_b:]
        span = min(len(a), len(b))
        if a[:span] != b[:span]:
            return f"'{name}' differs beyond a frame shift"
        if max(counts_a or [0]) != max(counts_b or [0]):
            return f"'{name}' peaks differ"

        # Counts agreeing is where the old check stopped, and where NE_C hid: its mirror matched
        # frame-for-frame on count while carrying no Color, Scale or Position at all. The channels
        # are compared under the same alignment the counts established.
        mismatch = _channel_mismatch(name, left[name], right[name], shift_a, shift_b)
        if mismatch is not None:
            return mismatch
    return "phased"


# --------------------------------------------------------------------------------------------
# Driving this ONE SYSTEM at a time is not a style choice.
#
# Every bridge exec runs to completion on the GameThread, so no frame boundary occurs inside one.
# DestroyComponent only *marks* a component for destruction and CollectGarbage is documented as
# "queued and happen at the end of the frame" -- neither takes effect until the frame ends. Running
# all 45 pairs in one call therefore left ~90 live Niagara systems simulating in the level at once,
# and the results were contaminated.
#
# Measuring one PAIR per call was not enough, and believing it was cost a long detour. It still put
# both sides of the pair in one exec, so the original was still simulating while the mirror was
# measured. That produced 11 "differ" verdicts -- all 10 N_MagicRuneCast_* and Teleport_Root -- for
# systems that are in fact identical: measured one system per call, N_MagicRuneCast_1 gives
# MainRune 24 / SecondRune 24 on BOTH sides, and Teleport_Root agrees on all 8 emitters. An entire
# "rebuilding in place leaves an emitter dead" investigation was chasing this artefact.
#
# Emitters that spawn nothing are what makes it visible: a zero-particle emitter is listed
# inconsistently by get_emitter_names(), so the contamination surfaces as "emitter set differs"
# rather than as a count that is merely wrong.
#
# So one system per call: side A, then side B, and the frame between the two calls is what releases
# side A. State survives because the bridge interpreter is persistent.
#
#     python .claude/skills/unreal-bridge/scripts/bridge.py --no-preflight exec-file <this file>
#     ...then, per index i:  exec "l3_side_a(i)"  and then  exec "l3_side_b(i)"
#     ...finally:            exec "l3_report()"
# --------------------------------------------------------------------------------------------

L3_PAIRS = []
L3_ROWS = []
L3_PENDING = {}


def l3_begin():
    """Discovers the pairs and clears any previous run. Returns how many there are."""
    global L3_PAIRS, L3_ROWS, L3_PENDING
    L3_PAIRS = sorted(_iter_mirror_pairs())
    L3_ROWS = []
    L3_PENDING = {}
    return len(L3_PAIRS)


def l3_side_a(index):
    """Measures the original. Every other side MUST be a separate call -- see the note above."""
    original, _ = L3_PAIRS[index]
    L3_PENDING[index] = {"a1": _counts_per_frame(original)}
    return f"{index + 1}/{len(L3_PAIRS)} a1"


def l3_side_b(index):
    """Measures the mirror."""
    _, mirror = L3_PAIRS[index]
    L3_PENDING.setdefault(index, {})["b"] = _counts_per_frame(mirror)
    return f"{index + 1}/{len(L3_PAIRS)} b"


def l3_side_c(index):
    """Measures the original a SECOND time, and that control is what decides the verdict.

    A system that uses randomness without Determinism does not replay the same way twice, so
    comparing it against its mirror answers a question it cannot answer. Eleven systems here are
    like that -- Up_Root and Magic_Explosion disagree with THEMSELVES beyond a frame shift -- and
    without this control they were reported as mirror differences.
    """
    original, _ = L3_PAIRS[index]
    captured = L3_PENDING.pop(index, {})
    control = _compare(captured.get("a1"), _counts_per_frame(original))

    if control != "exact":
        # Values are undecidable, but an attribute being absent is structural and survives
        # randomness -- so it is still judged, and only then does the pair become undecidable.
        absence = _absence_mismatch(captured.get("a1"), captured.get("b"))
        verdict = absence if absence is not None \
            else "nondeterministic (the original differs from itself)"
    else:
        verdict = _compare(captured.get("a1"), captured.get("b"))

    L3_ROWS.append((original.rsplit("/", 1)[-1], verdict))
    print(f"L3 {index + 1}/{len(L3_PAIRS)} {verdict:<50} {original}")
    return verdict


def l3_report():
    """Writes the markdown table and returns the summary line."""
    exact = sum(1 for _, v in L3_ROWS if v == "exact")
    phased = sum(1 for _, v in L3_ROWS if v == "phased")
    undecidable = sum(1 for _, v in L3_ROWS if v.startswith("nondeterministic"))
    failed = len(L3_ROWS) - exact - phased - undecidable

    out = os.path.join(unreal.Paths.project_saved_dir(), "DreamFX", "l3-report.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(f"# L3 runtime equivalence\n\n{FRAMES} frames at {DELTA:.4f}s, fixed step.\n")
        handle.write("One pair per editor frame, so a pair is never measured alongside another.\n\n")
        handle.write(f"**{exact} exact, {phased} phase-shifted, {failed} differ, "
                     f"{undecidable} undecidable** over {len(L3_ROWS)} pair(s).\n\n")
        handle.write("Undecidable means the original does not replay the same way twice, so the "
                     "mirror cannot be judged against it -- not that the two disagree.\n\n")
        handle.write("| asset | verdict |\n| --- | --- |\n")
        for name, verdict in L3_ROWS:
            handle.write(f"| `{name}` | {verdict} |\n")

    summary = (f"=== L3: {exact} exact, {phased} phased, {failed} differ, {undecidable} undecidable "
               f"over {len(L3_ROWS)} pair(s) -> {out} ===")
    print(summary)
    return summary


print(f"l3 harness loaded: {l3_begin()} pair(s). "
      "Per pair call l3_side_a(i), l3_side_b(i), l3_side_c(i) -- separate calls -- then l3_report().")
