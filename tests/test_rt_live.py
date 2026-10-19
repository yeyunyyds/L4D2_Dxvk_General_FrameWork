from pathlib import Path
import copy
import hashlib
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_rt_live import analyze, FRAMES, STAGES, MATERIALS, CONFIG

class LiveEvidence(unittest.TestCase):
 def fixture(self,root):
  events=[dict(event=name,code=0) for name in ('post_device_configuration_requested','api_mesh_policy')]
  events += [dict(event='model_path_config_requested',after_present=f,key=k,value='False',code=0) for f in FRAMES[:-1] for k in CONFIG]
  for frame in range(1,601):
   total=max(0,min(frame,540)-180);done=total if frame>=542 else max(0,total-1)
   events.append(dict(event='api_mesh_frame',frame=frame,submitted=total,created=total,reused=0,destroyed=done,entries=total-done,bytes=(total-done)*216,peak_bytes=432 if total>1 else total*216,completed_through=max(0,min(frame-2,540)),pending_fences=0 if frame>=542 else min(total,2)))
  identity=[float(i%5==0) for i in range(16)]
  for index,(frame,stage,material) in enumerate(zip(FRAMES,STAGES,MATERIALS)):
   n=int(240<=frame<=540);events.append(dict(event='api_model_path_readback',frame=frame,stage=stage,material=material,submission_samples=n,submission_sample_omitted=0,submitted_total=max(0,min(frame,540)-180),live_entries=int(frame<542 and frame>180)))
   if not n:continue
   raw=struct.pack('<24f',0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,0,0,1,0,0,1,0,0,0);indices=struct.pack('<3H',0,1,2)
   row=dict(event='api_model_sample',frame=frame,ordinal=index,stage=stage,material=material,shader='NekoToon',kind=2,topology=4,vertices=3,triangles=1,index_bytes=2,cull=1,ignored_category_at_original_draw=8,gpu_upload='pinned_CreateMesh_synchronous_copy_queued_owned_buffers',world=identity,view=identity,projection=identity,vertices_sha256=hashlib.sha256(raw).hexdigest(),indices_sha256=hashlib.sha256(indices).hexdigest(),payload_saved=frame in (240,300),files=[])
   if row['payload_saved']:
    for suffix,data in [('vertices.bin',raw),('indices.bin',indices)]:
     name='api-'+str(index)+'-'+suffix;(root/name).write_bytes(data);row['files'].append(dict(file=name,size=len(data),sha256=hashlib.sha256(data).hexdigest()))
   events.append(row)
  return events
 def v17_fixture(self,root):
  events=self.fixture(root);events.append(dict(event='world_coverage_revision',code=0,value='17'))
  for e in events:
   if e['event']=='api_mesh_frame':
    e.update(frame_draws=int(181<=e['frame']<=540),frame_new_bytes=216*int(181<=e['frame']<=540),frame_omitted=0,omitted_total=0,omission_reasons={k:0 for k in ('draw_limit','frame_geometry_bytes','live_geometry_bytes','mesh_entries','gpu_backlog')})
   if e['event']=='api_model_path_readback':e.update(previous_samples=int(e['frame'] in (240,300)),previous_sample_omitted=0,omitted_total=0)
  for frame in (240,300):
   original=next(e for e in events if e['event']=='api_model_sample' and e['frame']==frame);previous=copy.deepcopy(original)
   previous.update(event='api_model_previous_sample',frame=frame-1,ordinal=original['ordinal']+1000)
   for item in previous['files']:
    old=item['file'];item['file']=old.replace('api-'+str(original['ordinal'])+'-','api-'+str(previous['ordinal'])+'-');(root/item['file']).write_bytes((root/old).read_bytes())
   events.append(previous)
  return events
 def test_v17_budget_omissions_continue_evidence_and_remain_partial_coverage(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=self.v17_fixture(root);r=analyze(root,events);self.assertTrue(r['pass'],r['errors']);self.assertEqual(r['previous_pose_samples'],2)
   for e in events:
    if e['event']=='api_mesh_frame' and e['frame']>=339:
     e['omitted_total']=1;e['omission_reasons']['draw_limit']=1;e['frame_omitted']=int(e['frame']==339)
    if e['event']=='api_model_path_readback' and e['frame']>=339:e['omitted_total']=1
   events.append(dict(event='api_model_budget_omitted',code=0,frame=339,reason='draw_limit'))
   r=analyze(root,events);self.assertTrue(r['pass'],r['errors']);self.assertEqual(r['budget_omissions'],1);self.assertEqual(r['model_budget_coverage'],'partial_due_to_budget')
   next(e for e in events if e['event']=='api_mesh_frame' and e['frame']==340)['frame_omitted']=1
   self.assertIn('live_omission_delta',analyze(root,events)['errors'])
 def test_v17_previous_pose_and_actual_api_error_are_not_hidden(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=self.v17_fixture(root)
   missing=[e for e in events if not(e['event']=='api_model_previous_sample' and e['frame']==239)]
   self.assertIn('live_previous_count_mismatch',analyze(root,missing)['errors'])
   events.append(dict(event='api_model_submit_failed',code=8,operation='create_mesh'))
   self.assertIn('api_model_submit_failed',analyze(root,events)['errors'])
 def test_complete_live_batch_still_requires_image_review(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);result=analyze(root,self.fixture(root));self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['gpu_image_acceptance'],'unrun')
 def test_corruption_ignore_missing_stage_and_retirement_fail_closed(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);original=self.fixture(root)
   variants=[]
   e=copy.deepcopy(original);next(x for x in e if x['event']=='api_model_sample')['ignored_category_at_original_draw']=0;variants.append((e,'original_model_not_isolated'))
   e=copy.deepcopy(original);next(x for x in e if x['event']=='api_mesh_frame' and x['frame']==181)['destroyed']=2;variants.append((e,'live_resource_accounting'))
   e=copy.deepcopy(original);next(x for x in e if x['event']=='api_mesh_frame' and x['frame']==600)['pending_fences']=9;variants.append((e,'live_resource_budget'))
   e=[x for x in original if not(x['event']=='api_model_sample' and x['frame']==420)];variants.append((e,'live_sample_count_mismatch'))
   e=copy.deepcopy(original);e.append(dict(event='api_model_ignore_restore_failed',code=1));variants.append((e,'api_model_ignore_restore_failed'))
   for events,error in variants:self.assertIn(error,analyze(root,events)['errors'])
   (root/'api-3-vertices.bin').write_bytes(bytes(96));self.assertIn('live_payload_digest',analyze(root,original)['errors'])
if __name__=='__main__':unittest.main()
