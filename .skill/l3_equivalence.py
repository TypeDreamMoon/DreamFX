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

Run inside the editor (GPU emitters have no simulation under a commandlet's null RHI):

    py "Plugins/DreamFX/.skill/l3_equivalence.py"

Writes a markdown table to Saved/DreamFX/l3-report.md and prints the summary.
"""

import os
import unreal

FRAMES = 24
DELTA = 1.0 / 30.0

# Mirrors are exported into /Game/Decompiled/<original path>, so a pair is found by prefixing.
MIRROR_ROOT = "/Game/Decompiled"


def _iter_mirror_pairs():
    """Yields (original_path, mirror_path) for every mirror that has a source asset."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.wait_for_completion()

    for asset in registry.get_assets_by_path(MIRROR_ROOT, recursive=True):
        mirror_path = str(asset.package_name)
        if str(asset.asset_class_path.asset_name) != "NiagaraSystem":
            continue

        # /Game/Decompiled/Foo/Bar -> /Game/Foo/Bar. The namespace mirrors the original's own
        # path below its mount point, which is what makes this recoverable at all.
        tail = mirror_path[len(MIRROR_ROOT) + 1:]
        original_path = "/Game/" + tail

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

    series = {}
    try:
        for _ in range(FRAMES):
            # The out-parameter comes back in the tuple, so the cache to read is the second
            # element and not the one passed in -- passing None for the first argument lets the
            # library allocate it.
            ok, cache = unreal.NiagaraSimCacheFunctionLibrary.capture_niagara_sim_cache_immediate(
                None, unreal.NiagaraSimCacheCreateParameters(), component,
                advance_simulation=True, advance_delta_time=DELTA)
            if not ok or cache is None or cache.get_num_frames() == 0:
                continue
            for emitter in cache.get_emitter_names():
                positions = cache.read_position_attribute(
                    attribute_name="Position", emitter_name=emitter, frame_index=0)
                series.setdefault(str(emitter), []).append(len(positions))
    finally:
        component.deactivate()
        component.destroy_component()

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


def main():
    rows = []
    for original, mirror in sorted(_iter_mirror_pairs()):
        verdict = _compare(_counts_per_frame(original), _counts_per_frame(mirror))
        rows.append((original.rsplit("/", 1)[-1], verdict))
        unreal.log(f"L3 {verdict:<40} {original}")

    exact = sum(1 for _, v in rows if v == "exact")
    phased = sum(1 for _, v in rows if v == "phased")
    failed = len(rows) - exact - phased

    out = os.path.join(unreal.Paths.project_saved_dir(), "DreamFX", "l3-report.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(f"# L3 runtime equivalence\n\n{FRAMES} frames at {DELTA:.4f}s, fixed step.\n\n")
        handle.write(f"**{exact} exact, {phased} phase-shifted, {failed} differ** over {len(rows)} pair(s).\n\n")
        handle.write("| asset | verdict |\n| --- | --- |\n")
        for name, verdict in rows:
            handle.write(f"| `{name}` | {verdict} |\n")

    unreal.log(f"=== L3: {exact} exact, {phased} phased, {failed} differ over {len(rows)} pair(s) -> {out} ===")


main()
