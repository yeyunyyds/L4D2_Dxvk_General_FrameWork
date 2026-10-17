from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_rt_world import analyze, decode_normal


class ReadbackEvidenceTests(unittest.TestCase):
    def test_pinned_octahedral_encoding_axes_and_nan_bits(self):
        for word,expected in [(0x7fff7fff,(0,0,1)),(0x7ffffffe,(1,0,0)),
                              (0x00007fff,(0,-1,0)),(0x7fff0000,(-1,0,0)),(0,(0,0,-1))]:
            for actual,wanted in zip(decode_normal(word),expected):self.assertAlmostEqual(actual,wanted)
        self.assertIsNone(decode_normal(0xffff7fff))
        self.assertIsNone(decode_normal(0x7fffffff))

    def test_v5_combines_lighting_geometry_camera_and_failure_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            events=[{'event':name,'code':0} for name in ('loaded_pinned_runtime','initialize_api_0_6_4','register_game_device','lighting_comparison_policy')]
            phases={30:'raster',120:'baseline_sphere',180:'strong_sphere',240:'distant'}
            identity=[float(i%5==0) for i in range(16)]
            for frame,phase in phases.items():
                for kind in (0,1,2):
                    name=f'world-{frame}-{kind}.rgba32f'
                    if kind==0:raw=struct.pack('<4f',frame/1000,frame/1000,frame/1000,1)*(640*400)
                    elif kind==1:raw=struct.pack('<4f',.5,0,0,1)*2+struct.pack('<4f',1,0,0,1)*(640*400-2)
                    else:raw=struct.pack('<4I',0x7fff7fff,0,0,1)+struct.pack('<4I',0,0,0,1)*(640*400-1)
                    (root/name).write_bytes(raw)
                    events.append(dict(event='world_readback',frame=frame,phase=phase,kind=kind,file=name,width=640,height=400,
                                       initialized_to_zero=True,written=True,copy_code=0,hresult=0))
                events.append(dict(event='world_camera_sample',frame=frame,available=True,view=identity,projection=identity))
                events.append(dict(event='lighting_phase_requested',after_present=frame,code=0,
                                   phase={30:'baseline_sphere',120:'strong_sphere',180:'distant',240:'distant'}[frame]))
            log=root/'world-runtime.jsonl'
            def inspect():
                log.write_text('\n'.join(json.dumps(e) for e in events));return analyze(root,previews=False)
            report=inspect()
            self.assertEqual(report['runtime_outputs'],'pass',report['errors'])
            self.assertTrue(report['lighting_comparison']['same_camera_at_readbacks'])
            self.assertTrue(report['nonzero_world_depth_and_normals'])
            normal=next(s for s in report['samples'] if s['frame']==120 and s['kind']==2)
            self.assertEqual(normal['decoded_normals']['valid_depth_mask_pixels'],2)
            self.assertEqual(normal['decoded_normals']['unique_words_on_depth_hits'],2)
            color=next(s for s in report['samples'] if s['frame']==180 and s['kind']==0)
            self.assertAlmostEqual(color['luminance']['mean'],.18,places=6)
            original=json.loads(json.dumps(events))
            for e in events:
                if e.get('event')=='lighting_comparison_policy':e['event']='coverage_comparison_policy'
                if e.get('event')=='lighting_phase_requested':
                    e['event']='coverage_phase_requested';e['phase']='baseline_world' if e['after_present']==30 else 'expanded_world'
                if e.get('event')=='world_readback':e['phase']='raster' if e['frame']==30 else 'baseline_world' if e['frame']==120 else 'expanded_world'
            hud=dict(event='hud_injection',ordinal=1000,scope_code=9,capture_hresult=0,apply_hresult=0,draw_hresult=0,restore_hresult=0,replaced_post=True)
            events.append(hud)
            coverage=inspect()
            self.assertEqual(coverage['runtime_outputs'],'pass',coverage['errors'])
            self.assertEqual(coverage['coverage_comparison']['hud_post_replacements'],1)
            hud['restore_hresult']=0x80004005
            self.assertIn('missing_or_failed_hud_injection',inspect()['errors'])
            hud['restore_hresult']=0
            events.append(dict(event='world_coverage_revision',code=0,value='7'))
            hud['phase']='expanded_world'
            self.assertIn('missing_baseline_or_expanded_hud_injection',inspect()['errors'])
            baseline_hud=dict(hud,phase='baseline_world',ordinal=999);events.append(baseline_hud)
            v7=inspect()
            self.assertEqual(v7['runtime_outputs'],'pass',v7['errors'])
            self.assertEqual(v7['coverage_comparison']['revision'],7)
            self.assertEqual(v7['coverage_comparison']['hud_phase_counts'],{'baseline_world':1,'expanded_world':1})
            baseline_hud['phase']='raster'
            self.assertIn('missing_baseline_or_expanded_hud_injection',inspect()['errors'])
            events[:]=original
            events[-1]['code']=1
            self.assertIn('incomplete_or_failed_lighting_phase_requests',inspect()['errors'])
            events[-1]['code']=0
            next(e for e in events if e.get('event')=='world_camera_sample' and e['frame']==240)['view']=[2.]*16
            self.assertFalse(inspect()['lighting_comparison']['same_camera_at_readbacks'])
            events[:]=[e for e in events if e.get('frame')!=180]
            self.assertEqual(inspect()['runtime_outputs'],'incomplete')

    def test_alpha_only_normals_are_not_geometry_even_with_nonzero_depth(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            events=[{'event':name,'code':0} for name in ('loaded_pinned_runtime','initialize_api_0_6_4','register_game_device')]
            for frame in (30,120,240):
                for kind in (0,1,2):
                    name=f'world-{frame}-{kind}.rgba32f'
                    pixel=struct.pack('<4f',1.,0.,0.,1.) if kind==1 else struct.pack('<4I',0,0,0,1)
                    (root/name).write_bytes(pixel*(640*400))
                    events.append(dict(event='world_readback',frame=frame,kind=kind,file=name,width=640,height=400,
                                       initialized_to_zero=True,written=True,copy_code=0,hresult=0))
            (root/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            report=analyze(root,previews=False)
            self.assertEqual(report['runtime_outputs'],'pass')
            self.assertFalse(report['nonzero_world_depth_and_normals'])
            normals=[s for s in report['samples'] if s['kind']==2]
            self.assertTrue(all(s['nonzero_values']>0 and s['nonzero_finite_rgb_values']==0 for s in normals))
            self.assertTrue(all(s['nonzero_rgb_words']==0 and s['first_rgba_u32']==[0,0,0,1] for s in normals))

    def test_api_success_and_unchanged_zero_buffers_do_not_prove_world_geometry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            events=[{'event':name,'code':0} for name in ('loaded_pinned_runtime','initialize_api_0_6_4','register_game_device')]
            for frame in (30,120,240):
                for kind in (0,1,2):
                    name=f'world-{frame}-{kind}.rgba32f'
                    (root/name).write_bytes(bytes(640*400*16))
                    events.append(dict(event='world_readback',frame=frame,kind=kind,file=name,width=640,height=400,
                                       initialized_to_zero=True,written=True,copy_code=0,hresult=0))
            (root/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            report=analyze(root,previews=False)
            self.assertEqual(report['runtime_outputs'],'pass')
            self.assertFalse(report['nonzero_world_depth_and_normals'])
            self.assertEqual(report['geometry_visual_review'],'incomplete')
            self.assertNotEqual(report['game_rt'],'pass')
            gated=[{'event':'startup_raster_policy','code':0},
                   {'event':'rtx.enableRaytracing','code':0,'value':'False'}]+events
            log=root/'world-runtime.jsonl'
            log.write_text('\n'.join(json.dumps(e) for e in gated))
            self.assertIn('missing_gated_rt_on_off_sequence',analyze(root,previews=False)['errors'])
            gated.insert(2,{'event':'raytracing_requested_on','code':0})
            gated.append({'event':'raytracing_requested_off','code':0})
            log.write_text('\n'.join(json.dumps(e) for e in gated))
            self.assertEqual(analyze(root,previews=False)['runtime_outputs'],'pass')
            gated[1]['value']='True';log.write_text('\n'.join(json.dumps(e) for e in gated))
            self.assertIn('startup_rt_not_disabled',analyze(root,previews=False)['errors'])
            (root/'world-240-2.rgba32f').write_bytes(struct.pack('<f',1))
            self.assertEqual(analyze(root,previews=False)['runtime_outputs'],'incomplete')

    def test_missing_runtime_or_success_marker_stays_incomplete(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            self.assertEqual(analyze(root)['runtime_outputs'],'incomplete')
            (root/'world-runtime.jsonl').write_text('{}\n')
            self.assertEqual(analyze(root)['runtime_outputs'],'incomplete')


if __name__=='__main__':unittest.main()
