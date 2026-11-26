import unittest
import json
import tempfile
from pathlib import Path
from analyze_pm4_profile import rank_opcodes, analyze_ranking_only

class Pm4ProfileTests(unittest.TestCase):
    def capture(self, rows):
        return {'frametime_summary': {'total_frames':10}, 'pm4_opcode_profile': {'enabled':True, 'opcodes':rows}}
    def row(self, opcode, inclusive, excluded, wait):
        return dict(opcode=opcode, calls=2, inclusive_ns=inclusive, excluded_union_ns=excluded,
                    exclusive_ns=inclusive-excluded, host_wait_union_ns=wait)
    def test_rank_by_exclusive_not_inclusive(self):
        rows=[self.row(0x2d,1000,990,900),self.row(0x69,100,0,0)]
        ranked=rank_opcodes([self.capture(rows),self.capture(rows)],{0x69:'IT_SET_CONTEXT_REG'})
        self.assertEqual(ranked[0]['opcode'],0x69)
        self.assertEqual(ranked[0]['calls_per_frame'],0.2)
        self.assertEqual(ranked[0]['exclusive_avg_ns'],50)
    def test_reject_overlapping_sum_and_invalid_exclusive(self):
        for row in (self.row(1,100,110,20),self.row(1,100,40,50),dict(self.row(1,100,40,20),exclusive_ns=70)):
            with self.subTest(row=row),self.assertRaises(ValueError):
                rank_opcodes([self.capture([row])],{})

    def test_ranking_only_excludes_pipeline_creation_without_claiming_fps_cost(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'stdout.log').write_text('StartLevel aerial_garden')
            for index in range(1,4):
                data=self.capture([self.row(0x35,100,40,20)])
                data['metadata']=dict(capture_state='Completed',real_duration_seconds=30)
                data['configuration']=dict(profile_pm4=True)
                data['graphics_workload']=dict(draws_per_frame=5000,shaders_compiled=0,pipelines_created=1 if index==1 else 0)
                (root/f'pm4_on_{index}.json').write_text(json.dumps(data))
            result=analyze_ranking_only(root)
            self.assertEqual(result['clean_profiled_captures'],['pm4_on_2','pm4_on_3'])
            self.assertEqual(result['excluded_captures'][0]['label'],'pm4_on_1')
            self.assertFalse(result['instrumentation_fps_cost_established'])
            self.assertNotIn('instrumentation_fps_change_percent',result)

if __name__=='__main__':
    unittest.main()
