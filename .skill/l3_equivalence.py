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
    for i in $(seq 0 44); do python $B --no-preflight exec "l3_step($i)"; done
    python $B --no-preflight exec "l3_report()"

GPU emitters report zero here even in the editor -- there is no readback of their particle buffer on
this path. That is symmetric between the two sides of every pair, so it costs coverage, not
correctness: a GPU-only difference is invisible to this check rather than misreported by it.

Writes a markdown table to Saved/DreamFX/l3-report.md.
"""

import os
import unreal

FRAMES = 24
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


def _counts_per_frame(system_path):
    """
    Particle count per emitter per frame, as {emitter_name: [n0, n1, ...]}.

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
                positions = captured.read_position_attribute(
                    attribute_name="Position", emitter_name=emitter, frame_index=0)
                series.setdefault(str(emitter), []).append(len(positions))
    finally:
        component.deactivate()
        # DestroyComponent takes the caller: "may not be used to destroy a component that is owned
        # by an actor unless the owning actor is calling the function". Spawning into the editor
        # world attaches to the level's WorldSettings, so that is the owner to name.
        component.destroy_component(component.get_owner())

    return series


def _align(values):
    """Drops leading zeros, so two identical runs sampled a frame apart compare equal."""
    for index, value in enumerate(values):
        if value != 0:
            return values[index:]
    return []


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
        a, b = _align(left[name]), _align(right[name])
        span = min(len(a), len(b))
        if a[:span] != b[:span]:
            return f"'{name}' differs beyond a frame shift"
        if sum(left[name]) == 0 and sum(right[name]) != 0:
            return f"'{name}' is empty in one side"
        if max(left[name] or [0]) != max(right[name] or [0]):
            return f"'{name}' peaks differ"
    return "phased"


# --------------------------------------------------------------------------------------------
# Driving this one pair at a time is not a style choice.
#
# Every bridge exec runs to completion on the GameThread, so no frame boundary occurs inside one.
# DestroyComponent only *marks* a component for destruction and CollectGarbage is documented as
# "queued and happen at the end of the frame" -- neither takes effect until the frame ends. Running
# all 45 pairs in one call therefore left ~90 live Niagara systems simulating in the level at once,
# and the results were contaminated: NS_Spawn_Ground_Root was reported as "emitter set differs" and,
# re-measured on its own, has the identical 9 emitters on both sides.
#
# So the loop lives in the caller, one pair per call, and the frames between calls are what actually
# releases the previous pair. State survives because the bridge interpreter is persistent.
#
#     python .claude/skills/unreal-bridge/scripts/bridge.py --no-preflight exec-file <this file>
#     ...then, per index i:  exec "l3_step(i)"
#     ...finally:            exec "l3_report()"
# --------------------------------------------------------------------------------------------

L3_PAIRS = []
L3_ROWS = []


def l3_begin():
    """Discovers the pairs and clears any previous run. Returns how many there are."""
    global L3_PAIRS, L3_ROWS
    L3_PAIRS = sorted(_iter_mirror_pairs())
    L3_ROWS = []
    return len(L3_PAIRS)


def l3_step(index):
    """Measures one pair. Prints the verdict so the caller sees progress."""
    original, mirror = L3_PAIRS[index]
    verdict = _compare(_counts_per_frame(original), _counts_per_frame(mirror))
    L3_ROWS.append((original.rsplit("/", 1)[-1], verdict))
    print(f"L3 {index + 1}/{len(L3_PAIRS)} {verdict:<44} {original}")
    return verdict


def l3_report():
    """Writes the markdown table and returns the summary line."""
    exact = sum(1 for _, v in L3_ROWS if v == "exact")
    phased = sum(1 for _, v in L3_ROWS if v == "phased")
    failed = len(L3_ROWS) - exact - phased

    out = os.path.join(unreal.Paths.project_saved_dir(), "DreamFX", "l3-report.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(f"# L3 runtime equivalence\n\n{FRAMES} frames at {DELTA:.4f}s, fixed step.\n")
        handle.write("One pair per editor frame, so a pair is never measured alongside another.\n\n")
        handle.write(f"**{exact} exact, {phased} phase-shifted, {failed} differ** over {len(L3_ROWS)} pair(s).\n\n")
        handle.write("| asset | verdict |\n| --- | --- |\n")
        for name, verdict in L3_ROWS:
            handle.write(f"| `{name}` | {verdict} |\n")

    summary = f"=== L3: {exact} exact, {phased} phased, {failed} differ over {len(L3_ROWS)} pair(s) -> {out} ==="
    print(summary)
    return summary


print(f"l3 harness loaded: {l3_begin()} pair(s). Call l3_step(i) per pair, then l3_report().")
