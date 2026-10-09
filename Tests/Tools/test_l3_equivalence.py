"""Regression tests for capture validity; no Unreal installation/import required."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.component = Mock()
        self.capture = Mock()
        registry = SimpleNamespace(wait_for_completion=lambda: None,
                                   get_assets_by_class=lambda *a, **kw: [])
        unreal = SimpleNamespace(
            TopLevelAssetPath=lambda *a: a,
            AssetRegistryHelpers=SimpleNamespace(get_asset_registry=lambda: registry),
            load_asset=lambda path: object(), UnrealEditorSubsystem=object,
            get_editor_subsystem=lambda cls: SimpleNamespace(get_editor_world=lambda: object()),
            Vector=lambda *a: a,
            NiagaraFunctionLibrary=SimpleNamespace(spawn_system_at_location=lambda *a, **kw: self.component),
            NiagaraSimCacheCreateParameters=lambda: None,
            NiagaraSimCacheFunctionLibrary=SimpleNamespace(
                create_niagara_sim_cache=lambda world: object(),
                capture_niagara_sim_cache_immediate=self.capture),
        )
        path = Path(__file__).resolve().parents[2] / '.skill' / 'l3_equivalence.py'
        spec = importlib.util.spec_from_file_location('dreamfx_l3', path)
        self.module = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {'unreal': unreal}), contextlib.redirect_stdout(io.StringIO()):
            spec.loader.exec_module(self.module)
        self.module.FRAMES = 3

    @staticmethod
    def empty_frame():
        return SimpleNamespace(get_num_frames=lambda: 1, get_emitter_names=lambda: [])

    def test_failed_capture_is_not_exact(self):
        self.capture.return_value = None
        a = self.module._counts_per_frame('/Game/A')
        b = self.module._counts_per_frame('/Game/B')
        self.assertIsNone(a)
        self.assertEqual(self.module._compare(a, b), 'asset would not capture')
        self.assertEqual(self.component.destroy_component.call_count, 2)

    def test_zero_frames_and_partial_capture_fail(self):
        self.capture.return_value = SimpleNamespace(get_num_frames=lambda: 0)
        self.assertIsNone(self.module._counts_per_frame('/Game/A'))
        self.capture.side_effect = [self.empty_frame(), None]
        self.assertIsNone(self.module._counts_per_frame('/Game/A'))
        self.assertEqual(self.component.destroy_component.call_count, 2)

    def test_successful_empty_system_remains_valid(self):
        self.capture.side_effect = [self.empty_frame() for _ in range(3)]
        self.assertEqual(self.module._counts_per_frame('/Game/A'), {})
        self.assertEqual(self.capture.call_count, 3)

    def test_failed_control_is_not_nondeterminism(self):
        self.module.L3_PAIRS = [('/Game/A', '/Game/B')]
        self.module.L3_PENDING = {0: {'a1': {}, 'b': {}}}
        with patch.object(self.module, '_counts_per_frame', return_value=None), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(self.module.l3_side_c(0), 'asset would not capture')


if __name__ == '__main__':
    unittest.main()
