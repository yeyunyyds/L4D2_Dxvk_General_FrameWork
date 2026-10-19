import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import analyze_rt_draw as check


class DrawObservation(unittest.TestCase):
    def world_fixture(self):
        identity=[float(i%5==0) for i in range(16)]
        projection=[1.,0,0,0,0,1.,0,0,0,0,-1.0001,-1.,0,0,-7.0007,0]
        declaration=bytes.fromhex('000000000200000000000c00020003000000180001000500000020000100050102000000030000010200100002000301ff00000011000000')
        snapshot=check.SNAPSHOT.pack(0x314f444c,2,42,1,50,11,1,*(identity*2+projection),0x1000,0x2000,0,0,640,360,0,1)
        material=check.MATERIAL.pack(0x480003,3,0,b'LightmappedGeneric_DX9')
        state=check.PIPELINE.pack(0,0,48,1,0,0,1,1,0,0,640,360,0,1,640,360,113,7)+declaration
        events=[(0,0,struct.pack('<IQQQII',2,42,7,10000000,240,30))]
        for i in range(240):
            changed=bytearray(snapshot);struct.pack_into('<Q',changed,16,i+1)
            converted=int(i>=30)
            events.extend([(1,i*2+1,bytes(changed)+material+struct.pack('<8I',2,4,0,0,3,0,1,0)+state),
                           (6,i*2+1,struct.pack('<8I',8 if converted else 1,9 if converted else 0,0,0,0,converted,1,0)),
                           (2,i*2+1,struct.pack('<I',0)),(3,i*2+2,struct.pack('<I',0))])
        events.append((8,0,struct.pack('<6QI',240,240,240,0,0,0,0)))
        return events

    def test_v14_world_target_requires_complete_420_presents(self):
        events=self.scene_fixture()
        events[0]=(0,0,struct.pack('<IQQQII',11,42,7,10000000,420,30))
        self.assertFalse(self.inspect(events)['pass'])
        extra=[]
        for i in range(240,420):
            original=events[1+239*4:1+240*4]
            for kind,uid,payload in original:
                data=bytearray(payload)
                if kind==1:struct.pack_into('<Q',data,16,i+1)
                extra.append((kind,i*2+(2 if kind==3 else 1),bytes(data)))
        events[-1:-1]=extra
        events[-1]=(8,0,struct.pack('<6QI',420,420,420,0,0,0,0))
        report=self.inspect(events)
        self.assertTrue(report['pass'],report['errors'])
        self.assertEqual(report['header']['coverage_revision'],14)
        self.assertEqual(report['footer']['presents'],420)

    def test_v15_world_target_requires_complete_600_presents(self):
        events=self.scene_fixture()
        events[0]=(0,0,struct.pack('<IQQQII',12,42,7,10000000,600,30))
        self.assertFalse(self.inspect(events)['pass'])
        extra=[]
        for i in range(240,600):
            original=events[1+239*4:1+240*4]
            for kind,uid,payload in original:
                data=bytearray(payload)
                if kind==1:struct.pack_into('<Q',data,16,i+1)
                extra.append((kind,i*2+(2 if kind==3 else 1),bytes(data)))
        events[-1:-1]=extra
        events[-1]=(8,0,struct.pack('<6QI',600,600,600,0,0,0,0))
        report=self.inspect(events)
        self.assertTrue(report['pass'],report['errors'])
        self.assertEqual(report['header']['coverage_revision'],15)
        self.assertEqual(report['footer']['presents'],600)

    def test_v16_trace_uses_new_stage_semantics_without_changing_snapshot_wire(self):
        events=self.scene_fixture()
        events[0]=(0,0,struct.pack('<IQQQII',13,42,7,10000000,600,30))
        for i in range(240,600):
            original=events[1+239*4:1+240*4]
            extra=[]
            for kind,uid,payload in original:
                data=bytearray(payload)
                if kind==1:struct.pack_into('<Q',data,16,i+1)
                extra.append((kind,i*2+(2 if kind==3 else 1),bytes(data)))
            events[-1:-1]=extra
        events[-1]=(8,0,struct.pack('<6QI',600,600,600,0,0,0,0))
        result=self.inspect(events)
        self.assertTrue(result['pass'],result['errors'])
        self.assertEqual(result['header']['coverage_revision'],16)
        self.assertEqual(result['footer']['presents'],600)
        events[0]=(0,0,struct.pack('<IQQQII',14,42,7,10000000,600,30))
        result=self.inspect(events);self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['header']['coverage_revision'],17)
        events[0]=(0,0,struct.pack('<IQQQII',15,42,7,10000000,600,30))
        result=self.inspect(events);self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['header']['coverage_revision'],18)

    def test_v2_world_control_and_exact_padded_declaration(self):
        r=self.inspect(self.world_fixture())
        self.assertTrue(r['pass'],r['errors'])
        self.assertEqual(r['world_normalization'],'pass')
        self.assertEqual(r['world_conversion']['converted'],210)
        self.assertEqual(r['game_rt'],'unrun')

    def coverage_fixture(self):
        events=self.world_fixture()
        events[0]=(0,0,struct.pack('<IQQQII',3,42,7,10000000,240,30))
        unlit=bytes.fromhex('000000000200000000000c000200030000001800010005000100000004000a01020000000300000102001000020003010300000000000002ff00000011000000')
        post=bytes.fromhex('000000000200000000000c000100050002000000030000010200100002000301ff00000011000000')
        for frame,decl,shader,fmt in ((121,unlit,'UnlitGeneric',0x80503),(122,post,'Neko_Engine_Post',0x80001)):
            index=1+(frame-1)*4;e,u,p=events[index];data=bytearray(p[:check.SNAPSHOT.size])
            if frame==122:
                struct.pack_into('<48f',data,40,*([float(i%5==0) for i in range(16)]*3))
            material=check.MATERIAL.pack(fmt,3,0,shader.encode())
            old_state=list(check.PIPELINE.unpack_from(p,check.SNAPSHOT.size+check.MATERIAL.size+32))
            old_state[2]=32;old_state[-1]=len(decl)//8
            if frame==122:old_state[6]=old_state[7]=0;old_state[-2]=21
            args=p[check.SNAPSHOT.size+check.MATERIAL.size:check.SNAPSHOT.size+check.MATERIAL.size+32]
            events[index]=(e,u,bytes(data)+material+args+check.PIPELINE.pack(*old_state)+decl)
            if frame==122:events[index+1]=(6,u,struct.pack('<8I',9,9,0,0,0,0,0,0))
        return events

    def test_v6_coverage_and_hud_are_bounded_by_phase_and_exact_layout(self):
        events=self.coverage_fixture();report=self.inspect(events)
        self.assertTrue(report['pass'],report['errors'])
        self.assertEqual(report['world_conversion']['hud_replaced'],1)
        old=events.copy();e,u,p=old[0];old[0]=(e,u,struct.pack('<I',2)+p[4:])
        self.assertFalse(self.inspect(old)['pass'])
        for frame in (121,122):
            at=1+(frame-1)*4;e,u,p=events[at];data=bytearray(p)
            data[-len(bytes.fromhex('ff00000011000000'))-3]^=1
            damaged=events.copy();damaged[at]=(e,u,bytes(data))
            self.assertFalse(self.inspect(damaged)['pass'])
        early=events.copy();a,b=1+118*4,1+121*4
        early[a:a+3]=events[b:b+3]
        self.assertFalse(self.inspect(early)['pass'])

    def tangent_fixture(self):
        events=self.coverage_fixture()
        events[0]=(0,0,struct.pack('<IQQQII',4,42,7,10000000,240,30))
        state_at=check.SNAPSHOT.size+check.MATERIAL.size+32
        for i,(e,u,p) in enumerate(events):
            if e==1:events[i]=(e,u,p[:state_at+check.PIPELINE.size]+struct.pack('<I',3)+p[state_at+check.PIPELINE.size:])
        # v6 features are in the baseline already. Keep their original precise
        # snapshots but use the destination frame's ordinal and RPC identity.
        for source,frame in ((121,31),(122,32)):
            at=1+(frame-1)*4;src=1+(source-1)*4;e,u,_=events[at];data=bytearray(events[src][2]);struct.pack_into('<Q',data,16,frame)
            events[at]=(e,u,bytes(data));events[at+1]=(6,u,events[src+1][2])
        layouts={123:(0x480033,64,'000000000200000000000c0002000300000018000100050000002000010005010000280002000600000034000200070002000000030000010200100002000301ff00000011000000'),
                 124:(0x2480033,80,'000000000200000000000c0002000300000018000100050000002000010005010000280001000502000030000200060000003c000200070002000000030000010200100002000301ff00000011000000')}
        for frame,(fmt,stride,hex_decl) in layouts.items():
            at=1+(frame-1)*4;e,u,p=events[at];decl=bytes.fromhex(hex_decl)
            state=list(check.PIPELINE_V7.unpack_from(p,state_at));state[2]=stride;state[17]=len(decl)//8
            events[at]=(e,u,p[:check.SNAPSHOT.size]+check.MATERIAL.pack(fmt,3,0,b'LightmappedGeneric')+
                        p[check.SNAPSHOT.size+check.MATERIAL.size:state_at]+check.PIPELINE_V7.pack(*state)+decl)
        return events

    def test_v7_tangent_phase_exact_layout_and_original_cull_attribution(self):
        events=self.tangent_fixture();report=self.inspect(events)
        self.assertTrue(report['pass'],report['errors'])
        self.assertEqual(report['header']['coverage_revision'],7)
        self.assertEqual(report['world_conversion']['hud_replaced'],2)
        groups=report['world_material_attribution']['groups']
        self.assertTrue(any(g['phase']=='baseline_world' and g['shader']=='UnlitGeneric' and g['converted']==1 for g in groups))
        tangent=[g for g in groups if g['vertex_format'] in ('0x480033','0x2480033')]
        self.assertEqual({g['stride'] for g in tangent},{64,80})
        self.assertTrue(all(g['converted']==1 and g['cull_mode']==3 and g['first_rpc_uid'] for g in tangent))
        state_at=check.SNAPSHOT.size+check.MATERIAL.size+32
        for frame in (123,124):
            at=1+(frame-1)*4;e,u,p=events[at]
            mutations=[]
            for offset,value in ((state_at+2*4,72),(state_at+4,2),(state_at+3*4,2),
                                 (state_at+check.PIPELINE_V7.size+8+4,5),
                                 (state_at+check.PIPELINE_V7.size-4,0),
                                 (check.SNAPSHOT.size+check.MATERIAL.size,3),
                                 (check.SNAPSHOT.size+check.MATERIAL.size+4,2)):
                damaged=bytearray(p);struct.pack_into('<I',damaged,offset,value);changed=events.copy();changed[at]=(e,u,bytes(damaged));mutations.append(changed)
            early=events.copy();dest=1+119*4;_,dest_uid,_=early[dest];data=bytearray(p);struct.pack_into('<Q',data,16,120)
            early[dest]=(e,dest_uid,bytes(data));mutations.append(early)
            for changed in mutations:self.assertFalse(self.inspect(changed)['pass'])

    def scene_fixture(self):
        events=self.tangent_fixture();events[0]=(0,0,struct.pack('<IQQQII',5,42,7,10000000,240,30))
        state_at=check.SNAPSHOT.size+check.MATERIAL.size+32
        for i,(e,u,p) in enumerate(events):
            if e==1:events[i]=(e,u,p[:state_at+check.PIPELINE_V7.size]+struct.pack('<3I',1,1,0xfefefefe)+p[state_at+check.PIPELINE_V7.size:])
        # Full v7 tangent coverage is already active in the v8 baseline.
        src,dst=1+123*4,1+39*4;e,u,_=events[dst];data=bytearray(events[src][2]);struct.pack_into('<Q',data,16,40);events[dst]=(e,u,bytes(data))
        layouts={123:(10,0x80001,'NekoSky','000000000200000000000c000100050002000000030000010200100002000301ff00000011000000'),
                 181:(11,0x80503,'VertexLitGeneric','000000000200000000000c0005000300000010000100050000000c00050006000100000004000a01020000000300000102001000050003010300000000000002ff00000011000000'),
                 182:(11,0xa0403,'VertexLitGeneric','000000000200000000000c0005000300000010000100050000000c0005000600020000000300000102001000050003010300000000000002ff00000011000000')}
        for frame,(decision,fmt,shader,text) in layouts.items():
            at=1+(frame-1)*4;e,u,p=events[at];decl=bytes.fromhex(text);snapshot=bytearray(p[:check.SNAPSHOT.size])
            if decision==11:
                world=[0.,1,0,0,-1,0,0,0,0,0,1,0,123,0,0,1];struct.pack_into('<16f',snapshot,40,*world)
            state=list(check.PIPELINE_V8.unpack_from(p,state_at));state[2]=32;state[6]=state[7]=int(decision==11);state[17]=len(decl)//8
            events[at]=(e,u,bytes(snapshot)+check.MATERIAL.pack(fmt,3,0,shader.encode())+p[check.SNAPSHOT.size+check.MATERIAL.size:state_at]+check.PIPELINE_V8.pack(*state)+decl)
            events[at+1]=(6,u,struct.pack('<8I',decision,9,0,0,0,1,1,0))
        return events

    def test_v8_scene_stages_exact_layouts_and_original_states(self):
        events=self.scene_fixture();report=self.inspect(events)
        self.assertTrue(report['pass'],report['errors']);self.assertEqual(report['header']['coverage_revision'],8)
        self.assertEqual(report['world_conversion']['sky_converted'],1);self.assertEqual(report['world_conversion']['rigid_converted'],2)
        groups=report['world_material_attribution']['groups']
        scene=[g for g in groups if g['shader'] in ('NekoSky','VertexLitGeneric')]
        self.assertEqual({g['phase'] for g in scene},{'expanded_sky','expanded_models'})
        self.assertTrue(all(g['lighting']==g['fog']==1 and g['original_category']==0xfefefefe for g in scene))
        self.assertTrue(any(g['phase']=='baseline_world' and g['stride']==80 and g['converted']==1 for g in groups))
        state_at=check.SNAPSHOT.size+check.MATERIAL.size+32
        for frame in (123,181,182):
            at=1+(frame-1)*4;e,u,p=events[at]
            for offset,fmt,value in ((state_at,'I',4096),(state_at+2*4,'I',36),(40,'f',2.),
                                     (check.SNAPSHOT.size,'Q',0x80407),(check.SNAPSHOT.size+8,'I',0),
                                     (check.SNAPSHOT.size-4,'f',.1),(state_at+check.PIPELINE_V8.size+12,'I',5)):
                data=bytearray(p);struct.pack_into('<'+fmt,data,offset,value);bad=events.copy();bad[at]=(e,u,bytes(data))
                self.assertFalse(self.inspect(bad)['pass'],(frame,offset))
            # Last frame of the preceding phase is too early, regardless of UID.
            dest_frame=120 if frame==123 else 180;dest=1+(dest_frame-1)*4;_,dest_uid,_=events[dest]
            data=bytearray(p);struct.pack_into('<Q',data,16,dest_frame);bad=events.copy();bad[dest]=(e,dest_uid,bytes(data));bad[dest+1]=(6,dest_uid,events[at+1][2])
            self.assertFalse(self.inspect(bad)['pass'])
        newer=events.copy();newer[0]=(0,0,struct.pack('<IQQQII',6,42,7,10000000,240,30))
        self.assertTrue(self.inspect(newer)['pass']);self.assertEqual(self.inspect(newer)['header']['coverage_revision'],9)
        no_world=events.copy()
        for i,(e,u,p) in enumerate(no_world):
            if e==6 and struct.unpack_from('<I',p)[0]==8:no_world[i]=(e,u,struct.pack('<8I',0,0,0,0,0,0,1,0))
        self.assertIn('scene_before_successful_world',self.inspect(no_world)['errors'])

    def test_v10_bulk_static_affine_cutout_and_unsafe_inputs(self):
        events=self.scene_fixture();events[0]=(0,0,struct.pack('<IQQQII',7,42,7,10000000,240,30))
        at=1+180*4;e,u,p=events[at];data=bytearray(p);state_at=check.SNAPSHOT.size+check.MATERIAL.size+32
        struct.pack_into('<f',data,40,2.);struct.pack_into('<f',data,40+4,0.);struct.pack_into('<f',data,40+16,0.);struct.pack_into('<f',data,40+20,1.5)
        struct.pack_into('<Q',data,check.SNAPSHOT.size,0xa0103);struct.pack_into('<I',data,state_at+5*4,1)
        events[at]=(e,u,bytes(data));events[at+1]=(6,u,struct.pack('<8I',12,9,0,0,0,1,1,0))
        r=self.inspect(events);self.assertTrue(r['pass'],r['errors']);self.assertEqual(r['header']['coverage_revision'],10)
        self.assertEqual(r['world_conversion']['bulk_static_converted'],1)
        groups=r['static_family_inventory']['groups'];self.assertTrue(any(g['alpha_test']==1 and g['converted']==1 for g in groups))
        for offset,fmt,value in ((40,'f',-2.),(state_at+4*4,'I',1),(state_at+7*4,'I',0),
            (state_at+3*4,'I',2),(state_at+check.PIPELINE_V8.size+8+6,'B',2)):
            bad=events.copy();changed=bytearray(data);struct.pack_into('<'+fmt,changed,offset,value);bad[at]=(e,u,bytes(changed))
            self.assertFalse(self.inspect(bad)['pass'],(offset,value))
        early=events.copy();dest=1+179*4;_,dest_uid,_=events[dest];changed=bytearray(data);struct.pack_into('<Q',changed,16,180)
        early[dest]=(e,dest_uid,bytes(changed));early[dest+1]=(6,dest_uid,events[at+1][2]);self.assertFalse(self.inspect(early)['pass'])

    def test_v12_trace_keeps_baseline_and_guards_new_revision(self):
        events=self.scene_fixture();events[0]=(0,0,struct.pack('<IQQQII',9,42,7,10000000,240,30))
        r=self.inspect(events);self.assertTrue(r['pass'],r['errors']);self.assertEqual(r['header']['coverage_revision'],12)
        at=1+180*4;e,u,payload=events[at];data=bytearray(payload)
        struct.pack_into('<64s',data,check.SNAPSHOT.size+16,b'NekoToon');events[at]=(e,u,bytes(data))
        events[at+1]=(6,u,struct.pack('<8I',12,4,0,0,0,0,1,0))
        r=self.inspect(events);self.assertTrue(r['pass'],r['errors']);self.assertTrue(any(g['shader']=='NekoToon' and g['converted']==0 for g in r['static_family_inventory']['groups']))
        events[at+1]=(6,u,struct.pack('<8I',12,9,0,0,0,1,1,0));self.assertFalse(self.inspect(events)['pass'])
    def test_v13_neko_conversion_is_phase_pipeline_and_layout_bounded(self):
        events=self.scene_fixture();events[0]=(0,0,struct.pack('<IQQQII',10,42,7,10000000,240,30))
        at=1+180*4;e,u,p=events[at];data=bytearray(p);state_at=check.SNAPSHOT.size+check.MATERIAL.size+32
        struct.pack_into('<64s',data,check.SNAPSHOT.size+16,b'NekoToon')
        declaration=b''.join(struct.pack('<HH4B',*x) for x in [(0,0,2,0,0,0),(0,12,6,0,1,0),(0,16,4,0,2,0),(0,24,1,0,5,0)])+bytes.fromhex('ff00000011000000')
        state=list(check.PIPELINE_V8.unpack_from(data,state_at));state[17]=len(declaration)//8
        data=data[:state_at]+check.PIPELINE_V8.pack(*state)+declaration
        events[at]=(e,u,bytes(data));events[at+1]=(6,u,struct.pack('<8I',12,9,0,0,0,1,1,0))
        r=self.inspect(events);self.assertTrue(r['pass'],r['errors']);self.assertEqual(r['header']['coverage_revision'],13)
        for offset,value in [(state_at+4*4,1),(state_at+7*4,0),(state_at+3*4,2)]:
            changed=bytearray(data);struct.pack_into('<I',changed,offset,value);bad=events.copy();bad[at]=(e,u,bytes(changed));self.assertFalse(self.inspect(bad)['pass'])
        old=events.copy();old[0]=(0,0,struct.pack('<IQQQII',9,42,7,10000000,240,30));self.assertFalse(self.inspect(old)['pass'])
        early=events.copy();dest=1+179*4;_,dest_uid,_=events[dest];changed=bytearray(data);struct.pack_into('<Q',changed,16,180)
        early[dest]=(e,dest_uid,bytes(changed));early[dest+1]=(6,dest_uid,events[at+1][2]);self.assertFalse(self.inspect(early)['pass'])
    def test_v11_header_and_guarded_bone_declaration(self):
        events=self.scene_fixture();events[0]=(0,0,struct.pack('<IQQQII',8,42,7,10000000,240,30))
        at=1+180*4;e,u,p=events[at];data=bytearray(p)
        events[at+1]=(6,u,struct.pack('<8I',12,9,0,0,0,1,1,0))
        r=self.inspect(events);self.assertTrue(r['pass'],r['errors']);self.assertEqual(r['header']['coverage_revision'],11)
        position=struct.pack('<HH4B',0,0,2,0,0,0);uv=struct.pack('<HH4B',0,24,1,0,5,0)
        weight=struct.pack('<HH4B',0,12,6,0,1,0);index=struct.pack('<HH4B',0,16,4,0,2,0);end=bytes.fromhex('ff00000011000000')
        declaration=position+weight+index+uv+end
        self.assertTrue(check.static_layout(declaration,32,True));self.assertFalse(check.static_layout(declaration,32))
        self.assertFalse(check.static_layout(position+index+uv+end,32,True))
        self.assertFalse(check.static_layout(position+weight+weight+index+uv+end,32,True))

    def test_static_inventory_includes_rejected_bone_and_cutout_families(self):
        events=self.scene_fixture();at=1+180*4;e,u,p=events[at];data=bytearray(p);state_at=check.SNAPSHOT.size+check.MATERIAL.size+32
        struct.pack_into('<I',data,state_at+5*4,1);struct.pack_into('<B',data,state_at+check.PIPELINE_V8.size+8+6,2)
        events[at]=(e,u,bytes(data));events[at+1]=(6,u,struct.pack('<8I',11,4,0,0,0,0,1,0))
        r=self.inspect(events);self.assertTrue(r['pass'],r['errors'])
        self.assertTrue(any(g['has_bone_fields'] and g['alpha_test'] and not g['converted'] for g in r['static_family_inventory']['groups']))
        self.assertEqual(r['static_family_inventory']['omitted_draws'],0)

    def test_world_report_cannot_hide_bad_uid_control_or_unsafe_material(self):
        original=self.world_fixture()
        wrong=original.copy();wrong[2]=(6,999,wrong[2][2])
        control=original.copy();control[2]=(6,control[2][1],struct.pack('<8I',8,9,0,0,0,1,1,0))
        material=original.copy();at=1+30*4;e,u,p=material[at];data=bytearray(p)
        struct.pack_into('<Q',data,check.SNAPSHOT.size,0x80407);material[at]=(e,u,bytes(data))
        for changed in (wrong,control,material):self.assertFalse(self.inspect(changed)['pass'])

    def fixture(self):
        matrix=[float(i%5==0) for i in range(16)]
        snapshot=check.SNAPSHOT.pack(0x314f444c,1,42,1,50,11,1,*(matrix*3),0x1000,0x2000,0,0,640,360,0,1)
        state=check.PIPELINE.pack(0,0,40,1,0,0,1,1,0,0,640,360,0,1,640,360,21,0)
        events=[(0,0,struct.pack('<IQQQ',1,42,7,10000000))]
        for i in range(120):
            changed=bytearray(snapshot);struct.pack_into('<Q',changed,16,i+1)
            events.extend([(1,i*2+1,bytes(changed)+struct.pack('<8I',2,4,0,0,3,0,1,0)+state),
                           (2,i*2+1,struct.pack('<I',0)),(3,i*2+2,struct.pack('<I',0))])
        events.append((8,0,struct.pack('<6QI',120,120,120,0,0,0,0)))
        return events

    def inspect(self,events):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'trace.bin'
            path.write_bytes(b''.join(check.RECORD.pack(e,32+len(p),u,19,0,i+1)+p for i,(e,u,p) in enumerate(events)))
            return check.analyze(path)

    def test_real_draw_result_correspondence_without_source_queue_claim(self):
        r=self.inspect(self.fixture())
        self.assertTrue(r['pass'],r['errors'])
        self.assertEqual(r['footer']['draws'],120)
        self.assertEqual(r['game_rt'],'unrun')
        self.assertEqual(r['source_queue_execution'],'unrun')
        self.assertEqual(r['source_parallel_preparation'],'unrun')

    def test_result_uid_missing_footer_and_truncation_do_not_pass(self):
        events=self.fixture()
        wrong=events.copy();wrong[2]=(2,1000,wrong[2][2])
        for changed in [wrong,events[:-1],events+[events[1]]]:
            self.assertFalse(self.inspect(changed)['pass'])

    def test_status_cannot_hide_invalid_matrix_or_viewport_mismatch(self):
        events=self.fixture();event,uid,payload=events[1]
        for offset,value in [(40,float('nan')),(40+32*4,0.0)]:
            damaged=bytearray(payload)
            if offset==40+32*4:
                damaged[offset:offset+64]=bytes(64)
            else:
                struct.pack_into('<f',damaged,offset,value)
            changed=events.copy();changed[1]=(event,uid,bytes(damaged))
            self.assertFalse(self.inspect(changed)['pass'])
        damaged=bytearray(payload);struct.pack_into('<I',damaged,check.SNAPSHOT.size+32+10*4,800)
        changed=events.copy();changed[1]=(event,uid,bytes(damaged))
        self.assertFalse(self.inspect(changed)['pass'])
        damaged=bytearray(payload);struct.pack_into('<I',damaged,check.SNAPSHOT.size+32,1)
        changed=events.copy();changed[1]=(event,uid,bytes(damaged))
        self.assertIn('pipeline_query_failure',self.inspect(changed)['errors'])

    def test_profile_failure_duplicate_and_reset_are_explicit(self):
        events=self.fixture();event,uid,payload=events[1]
        damaged=bytearray(payload);struct.pack_into('<I',damaged,36,3)
        changed=events.copy();changed[1]=(event,uid,bytes(damaged))
        self.assertFalse(self.inspect(changed)['pass'])
        changed=events.copy();changed[4]=(1,changed[4][1],payload)
        self.assertFalse(self.inspect(changed)['pass'])
        changed=events.copy();changed[3]=(4,changed[3][1],struct.pack('<I',0))
        self.assertFalse(self.inspect(changed)['pass'])


if __name__=='__main__':unittest.main()
