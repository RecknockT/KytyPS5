import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from analyze_effect_profile import analyze


class EffectAnalysisTests(unittest.TestCase):
    def sample(self, frame, calls, cpu, gpu, readback=True):
        row = dict(kind=2, shader='1234', pixel='0', targets=0, calls=calls,
                   cpu_ns=cpu, prep_ns=cpu//2, gpu_samples=calls, gpu_sample_ns=gpu)
        if readback:
            row.update(readback_ns=cpu//4, readback_count=calls)
        return dict(frame=frame, host_ns=frame*1_000_000, rows=[row], timestamp_bits=64,
                    completed_batches=calls, dropped=0, submission_interval_ns=gpu)

    def run_analysis(self, snapshots, tail=''):
        with tempfile.TemporaryDirectory(prefix='kyty_effect_analysis_') as temporary:
            directory=Path(temporary)
            (directory/'effects.jsonl').write_text(''.join(json.dumps(s)+'\n' for s in snapshots)+tail)
            with contextlib.redirect_stdout(io.StringIO()):
                return analyze(directory, 600)

    def test_interval_uses_deltas_and_weighted_sample_average(self):
        result=self.run_analysis([self.sample(120,20,2000,4000), self.sample(720,80,8000,16000)])
        self.assertEqual(result['frames'],600)
        self.assertAlmostEqual(result['rows'][0]['calls_per_frame'],0.1)
        self.assertEqual(result['rows'][0]['gpu_samples'],60)
        self.assertAlmostEqual(result['rows'][0]['gpu_sample_avg_us'],0.2)
        self.assertAlmostEqual(result['readback_ms_per_frame'],0.0000025)

    def test_incomplete_tail_keeps_complete_records(self):
        result=self.run_analysis([self.sample(120,20,2000,4000), self.sample(720,80,8000,16000)],'{"frame":')
        self.assertTrue(result['incomplete_tail'])
        self.assertEqual(result['end_frame'],720)

    def test_absent_readback_data_is_unknown(self):
        result=self.run_analysis([self.sample(120,20,2000,4000,False), self.sample(720,80,8000,16000,False)])
        self.assertFalse(result['readback_attribution_available'])
        self.assertIsNone(result['readback_ms_per_frame'])
        self.assertIsNone(result['rows'][0]['readback_ms_per_frame'])

    def test_absent_gpu_samples_are_unknown(self):
        snapshots=[self.sample(120,20,2000,0), self.sample(720,80,8000,0)]
        for snapshot in snapshots:
            snapshot['rows'][0]['gpu_samples']=0
            snapshot['timestamp_bits']=0
        result=self.run_analysis(snapshots)
        self.assertIsNone(result['rows'][0]['gpu_sample_avg_us'])

    def test_decreasing_counters_are_rejected(self):
        with self.assertRaisesRegex(ValueError,'Counters decreased'):
            self.run_analysis([self.sample(120,80,8000,16000), self.sample(720,20,2000,4000)])


if __name__=='__main__':
    unittest.main()
