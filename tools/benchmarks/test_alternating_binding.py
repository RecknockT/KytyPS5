import json
import tempfile
import unittest
from pathlib import Path

from analyze_alternating_binding import analyze


class AlternatingBindingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.write('run.json', dict(sha='test', executableHash='a' * 64, pid=1))
        self.write('replay.json', [])
        (self.root / 'stdout.log').write_text('load gate passed sky_garden_started\n')
        for enabled in (False, True):
            for index in range(1, 4):
                frames = 630 if enabled else 600
                self.write(f"binding_{'on' if enabled else 'off'}_{index}.json", {
                    'configuration': dict(binding_reuse=enabled, descriptor_cache=True),
                    'metadata': dict(capture_state='Completed', real_duration_seconds=30),
                    'frametime_summary': dict(total_frames=frames, p95_frame_ms=55),
                    'graphics_workload': dict(shaders_compiled=0, pipelines_created=0, draws_per_frame=5200),
                    'pipeline_stage_timings': {k: dict(ms_per_frame=5) for k in ('DescriptorPrep', 'RenderStatePrep')},
                    'gpu_waits_breakdown': {k: dict(thread_accumulated_ms_per_frame=10) for k in ('PriorityOperation', 'BufferReadback')},
                })

    def write(self, name, value):
        (self.root / name).write_text(json.dumps(value))

    def test_complete_alternation(self):
        result = analyze(self.root)
        self.assertAlmostEqual(result['median_fps_change_percent'], 5)
        self.assertTrue(result['all_pairs_improved'])

    def test_missing_level_gate(self):
        (self.root / 'stdout.log').write_text('worldmap')
        with self.assertRaises(ValueError):
            analyze(self.root)

    def test_contaminated_or_wrong_capture(self):
        name = 'binding_on_1.json'
        original = json.loads((self.root / name).read_text())
        for section, key, value in (
            ('configuration', 'binding_reuse', False),
            ('configuration', 'descriptor_cache', False),
            ('metadata', 'capture_state', 'Interrupted'),
            ('graphics_workload', 'shaders_compiled', 1),
            ('graphics_workload', 'draws_per_frame', 900),
            ('graphics_workload', 'draws_per_frame', 6000),
        ):
            with self.subTest(section=section, key=key, value=value):
                modified = json.loads(json.dumps(original))
                modified[section][key] = value
                self.write(name, modified)
                with self.assertRaises(ValueError):
                    analyze(self.root)
                self.write(name, original)


if __name__ == '__main__':
    unittest.main()
