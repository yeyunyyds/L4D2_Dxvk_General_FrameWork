"""Install the paired observer, collect one F9 burst, and retain one result ZIP."""
from array import array
import math
import argparse
import hashlib
import json
import lzma  # Required by lossless result ZIP compression; checked in packaged Python.
import os
import re
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from analyze_rt_draw import analyze
from rt_game_probe import WindowsProcess, collect
from rt_preflight import game_pids
from rt_preflight import ensure_runtime, verify_load, check_file
from analyze_rt_world import analyze as analyze_world
from analyze_rt_scene import analyze as analyze_scene
from rt_gpu_telemetry import Sampler
from analyze_rt_runtime import analyze as analyze_runtime

TARGETS=('bin/dxvk_d3d9.dll','bin/.l4d2bridge/L4D2Bridge64.exe')
SUPPORT_FILES=('integration/rt/baseline.json','research/rt-source-audit.json',
               'research/rt-source-queue-sdk.json')


def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):Path(path).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def completed_preview(path,session):
    try:marker=json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError,json.JSONDecodeError):return False
    return marker.get('session')==session and marker.get('complete') is True


def verify_package(root):
    manifest=json.loads((root/'draw-observer-manifest.json').read_text(encoding='utf-8'))
    for name,entry in manifest['files'].items():
        if digest(root/name)!=entry['sha256']:raise ValueError('Package checksum mismatch: '+name)
    for name in SUPPORT_FILES:
        if name not in manifest['files']:raise ValueError('Required collector configuration missing from manifest: '+name)
        json.loads((root/name).read_text(encoding='utf-8'))
    baseline=json.loads((root/SUPPORT_FILES[0]).read_text(encoding='utf-8'))
    if not baseline.get('compat_commit'):raise ValueError('Collector reference commit missing')
    if manifest.get('profile')=='world_pilot':
        name='integration/rt/world-runtime.json'
        if name not in manifest['files']:raise ValueError('World runtime catalog missing from manifest')
        runtime=json.loads((root/name).read_text(encoding='utf-8'))
        persistent=runtime.get('runtime_patches',{}).get('persistent_mesh',{})
        if (runtime.get('scope')!='rt_prerequisites' or runtime['runtime_files']['d3d9.dll']['sha256']!=
                '0ecebaf16bb4f89575354f49cc6a23ccc4689e20aa3f59fe9e2844871aa5ef9b' or
                persistent.get('export')!='remixapi_LdbUpdateMesh' or
                persistent.get('topology_identity')!='first_frame_upsert_stable_indices_descriptor_dynamic_vertex_generation_forced_dynamic_blas'):
            raise ValueError('World runtime pin differs')
    return manifest


def replace_copy(source,target):
    target=Path(target)
    with tempfile.NamedTemporaryFile(prefix='rt-observer-',suffix='.tmp',dir=target.parent,delete=False) as temporary:
        name=Path(temporary.name)
    try:
        shutil.copy2(source,name);os.replace(name,target)
    finally:
        name.unlink(missing_ok=True)


def install(root,game,manifest):
    receipt=root/'installed-observer.json'
    if all((game/name).is_file() and digest(game/name)==manifest['binaries'][name]['sha256'] for name in TARGETS):
        if receipt.is_file():
            old=json.loads(receipt.read_text(encoding='utf-8'))
            if Path(old['game'])!=game:raise ValueError('Installation receipt belongs to another game directory')
            return old
    for name in TARGETS:
        if not (game/name).is_file():raise ValueError('LDB 1.3.2 installation not found: '+name)
        if digest(root/'payload'/name)!=manifest['binaries'][name]['sha256']:raise ValueError('Observer binary checksum mismatch: '+name)
    backup=root/'backups'/secrets.token_hex(8);backup.mkdir(parents=True)
    old={'game':str(game),'files':{},'scope':'paired_draw_observer_only'}
    # Carry the original backup forward when the user extracts a new test
    # beside an older one. Never turn a previous observer into the "original".
    inherited=None
    receipts=[receipt]+[p/'installed-observer.json' for p in list(root.parent.iterdir())[:128] if p.is_dir() and p!=root]
    for candidate in receipts:
        if not candidate.is_file():continue
        try:
            previous=json.loads(candidate.read_text(encoding='utf-8'))
            if Path(previous['game'])!=game:continue
            if not all(digest(game/n)==previous['files'][n]['installed_sha256'] for n in TARGETS):continue
            if not all(digest(previous['files'][n]['backup'])==previous['files'][n]['previous_sha256'] for n in TARGETS):continue
            if inherited and any(inherited['files'][n]['previous_sha256']!=previous['files'][n]['previous_sha256'] for n in TARGETS):
                raise ValueError('Conflicting original backups; restore the earlier observer first')
            inherited=previous
        except (OSError,KeyError,TypeError,json.JSONDecodeError):continue
    known_observers={'59053aa7c512601ac1c623e31dd3acdcfc9197fb921c917ef8f79c3b0510254e',
                     'c220626110df028be42f3df7732b116ae8c04c341fce8dd695362bc608b43b64',
                     '26507cdd3315268f6beb8c03ecf98b113c0135f423f5209d1f5ab17cdec80c11',
                     '9b8436a1832eb1133ab2a0d94989e84c24bb633e55c606a7baec009d6a096d15',
                     'b0bbc701404bfeaa1be6f51183716ce98e79884209818b35b55f5267131b8f8a',
                     '27349fe2def9ade2aaa637b32189db09e71fc41f79afdf9e06fd8e3c2e6c3d87',
                     '3f1a899dd2021610c17bf32cb076b749293910b5fe6f4681b2695a1dfbb57fbb',
                     'd280301fa0dd70084aff94bdccad08f68d5fbe76fa1d3c69d30ea2f14b92094a',
                     'd9f6cbefa7e090deceb5d773f8999b34eaa5c9910cec0a56a8abbacf6e6c779f',
                     '0a579c30ef02a20be14ca4250c24f5ad4165309edd78b8df8a04e5741549bff3',
                     'e06caf633c2a3e1994c405470717dfbe73d3f1a877ea9b94e153e25e117ccb18',
                     '6036c11994ba91e881f255d160b91bf3906775c30af18bd1bfdac42e5c8c7773',
                     '5ef8cd2d0c00070ec10d6f50f28ecd2707a4e2a8cc3fe3105332669b1c904341',
                     'b39a73c667023225dd5fbd666357ea854386f08928d009182e2d2aff62bd6e58',
                     '719a95eaa9f7defa7b7dc0f8987a9227fcdc35b89663f6a990cb8f86895b17c9',
                     manifest['binaries'][TARGETS[0]]['sha256']}
    if inherited is None and digest(game/TARGETS[0]) in known_observers:
        raise ValueError('Earlier observer found without its recovery receipt. Restore it from its original package folder first.')
    for name in TARGETS:
        source=Path(inherited['files'][name]['backup']) if inherited else game/name
        destination=backup/name;destination.parent.mkdir(parents=True)
        shutil.copy2(source,destination)
        old['files'][name]={'backup':str(destination),'previous_sha256':digest(destination),
                            'installed_sha256':manifest['binaries'][name]['sha256']}
    changed=[];before={}
    for name in TARGETS:
        rollback=backup/'rollback'/name;rollback.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(game/name,rollback);before[name]=rollback
        old['files'][name]['rollback_sha256']=digest(rollback)
    # All copies exist and the recovery receipt is saved before mutation.
    save(receipt,old)
    try:
        for name in TARGETS:
            replace_copy(root/'payload'/name,game/name);changed.append(name)
        for name in TARGETS:
            if digest(game/name)!=old['files'][name]['installed_sha256']:raise ValueError('Installed pair verification failed')
    except Exception:
        for name in reversed(changed):
            if digest(game/name)==old['files'][name]['installed_sha256']:
                replace_copy(before[name],game/name)
        for name in TARGETS:old['files'][name]['installed_sha256']=digest(game/name)
        save(receipt,old)
        raise
    return old


def runtime_for_world(root,notify,payload_cache=None):
    catalog=json.loads((root/'integration/rt/world-runtime.json').read_text(encoding='utf-8'))
    # Reuse only complete, SHA-verified sibling preflight caches.
    destination=root/'cache/runtime'
    if not all(check_file(destination/n,e) for n,e in catalog['runtime_files'].items()):
        for package in list(root.parent.iterdir())[:128]:
            cache=package/'cache/runtime'
            if package!=root and cache.is_dir() and all(check_file(cache/n,e) for n,e in catalog['runtime_files'].items()):
                notify('Reusing verified runtime files from the earlier preflight package.')
                for name in catalog['runtime_files']:
                    target=destination/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(cache/name,target)
                break
    runtime=ensure_runtime(root,catalog,payload_cache=payload_cache,notify=notify)
    verify_load(runtime)
    return runtime


def restore(root):
    receipt=json.loads((root/'installed-observer.json').read_text(encoding='utf-8'));game=Path(receipt['game'])
    for name,entry in receipt['files'].items():
        if digest(entry['backup'])!=entry['previous_sha256']:raise ValueError('Backup checksum mismatch')
        if digest(game/name) not in (entry['installed_sha256'],entry['previous_sha256'],entry.get('rollback_sha256')):raise ValueError('Game files changed after installation; preserve them: '+name)
    for name,entry in receipt['files'].items():
        if digest(game/name)!=entry['previous_sha256']:replace_copy(Path(entry['backup']),game/name)
    return game


def locate_game(explicit,pids,root):
    if explicit:return Path(explicit).resolve()
    receipt=root/'installed-observer.json'
    if receipt.is_file():return Path(json.loads(receipt.read_text(encoding='utf-8'))['game'])
    if len(pids)==1:
        with WindowsProcess(pids[0]) as process:
            main=next((m for m in process.modules() if m['name']=='left4dead2.exe'),None)
            if main:return Path(main['path']).parent
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r'Software\Valve\Steam') as key:
        steam=Path(winreg.QueryValueEx(key,'SteamPath')[0])
    libraries=[steam]
    text=(steam/'steamapps/libraryfolders.vdf').read_text(encoding='utf-8')
    import re
    libraries.extend(Path(x.replace('\\\\','\\')) for x in re.findall(r'"path"\s*"([^"]+)"',text))
    candidates=[p/'steamapps/common/Left 4 Dead 2' for p in libraries if (p/'steamapps/appmanifest_550.acf').is_file()]
    candidates=list(dict.fromkeys(candidates))
    if len(candidates)!=1:raise ValueError('Cannot select one L4D2 installation; use --game <game directory>')
    return candidates[0]


def wait_stopped(notify,timeout=300):
    deadline=time.monotonic()+timeout;printed=False
    while True:
        diagnostics={};pids=game_pids(diagnostics)
        active=[p for p in diagnostics.get('related_processes',[]) if p['exe'].casefold() in ('left4dead2.exe','l4d2bridge64.exe')]
        if not pids and not active:return
        if not printed:notify('Please exit L4D2 normally. Waiting for the game and Host to stop; keep this window open...');printed=True
        if time.monotonic()>deadline:raise TimeoutError('Game or Host is still running; operation stopped and available results retained')
        time.sleep(1)


def archive_result(directory,report):
    files=[p for p in directory.rglob('*') if p.is_file() and 'runtime-textures' not in p.relative_to(directory).parts]
    if sum(p.stat().st_size for p in files)>512*1024*1024:
        report['collection']='incomplete';report['errors'].append('result_budget_exceeded')
    report['archive_compression']='zip_lzma_lossless'
    save(directory/'summary.json',report)
    output=directory.with_suffix('.zip')
    temporary=output.with_name(output.name+'.tmp')
    try:
        with zipfile.ZipFile(temporary,'w',zipfile.ZIP_LZMA) as z:
            used=0;omitted=[]
            for path in sorted(directory.rglob('*')):
                if not path.is_file() or 'runtime-textures' in path.relative_to(directory).parts:continue
                if used+path.stat().st_size>512*1024*1024 and path.name!='summary.json':
                    omitted.append(str(path.relative_to(directory)));continue
                z.write(path,path.relative_to(directory));used+=path.stat().st_size
            z.writestr('omitted-files.json',json.dumps(omitted))
        os.replace(temporary,output)
    finally:
        temporary.unlink(missing_ok=True)
    return output


def collect_game_logs(game,directory,since):
    inventory={'association':'game directory and mtime only; PID identity requires log review','copied':[],'omitted':[]}
    used=0
    for index,folder in enumerate((game/'rtx-remix/logs',game/'bin/rtx-remix/logs',game/'bin/.l4d2bridge/rtx-remix/logs')):
        if not folder.is_dir():continue
        for source in list(folder.iterdir())[:128]:
            if source.is_symlink() or not source.is_file() or source.suffix.lower() not in ('.log','.dmp'):continue
            if source.stat().st_mtime<since-2:continue
            size=source.stat().st_size
            if size>16*1024*1024 or used+size>32*1024*1024:
                inventory['omitted'].append(str(source.relative_to(game)));continue
            target=directory/'game-logs'/str(index)/source.name;target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(source,target);used+=size
            inventory['copied'].append({'game_relative':str(source.relative_to(game)),'file':str(target.relative_to(directory)),
                                        'sha256':digest(target),'size':target.stat().st_size})
    save(directory/'game-log-inventory.json',inventory)


def review_exit_logs(directory,game_pid=None):
    """Keep completed data separate from crashes during later process teardown."""
    directory=Path(directory);faults=[];cleanup=set();unbound=[];remaining=0
    inventory=directory/'game-log-inventory.json'
    entries=json.loads(inventory.read_text(encoding='utf-8')).get('copied',[]) if inventory.is_file() else []
    for item in entries:
        name=item['file'].replace('\\','/');relative=Path(name)
        if relative.is_absolute() or '..' in relative.parts:raise ValueError('Unsafe exit log path')
        path=directory/relative
        if path.suffix.lower()!='.log':continue
        if path.stat().st_size>16*1024*1024 or digest(path)!=item['sha256']:raise ValueError('Exit log hash/size mismatch')
        text=path.read_text(encoding='utf-8',errors='replace')
        if 'Shutdown cleanup successful' in text and path.name in ('bridge32.log','bridge64.log'):cleanup.add(path.name)
        for block in text.split('===== L4D2_BRIDGE_EXCEPTION_BEGIN =====')[1:17]:
            block=block.split('===== L4D2_BRIDGE_EXCEPTION_END =====')[0]
            identity=re.search(r'side=(client|host) pid=(\d+)',block);code=re.search(r'exception_code=(0x[0-9a-fA-F]+)',block)
            if not identity or not code:unbound.append(name);continue
            side,pid=identity.group(1),int(identity.group(2))
            if side=='client' and game_pid is not None and pid!=game_pid:unbound.append(name);continue
            target=re.search(r'target_address=(0x[0-9a-fA-F]+)',block)
            modules=re.findall(r'frame\d+ address=\S+ module=([^\s]+).*?module_offset=(0x[0-9a-fA-F]+)',block)
            faults.append({'file':name,'side':side,'pid':pid,'code':code.group(1),'target':target.group(1) if target else None,
                           'named_frames':[{'module':m,'offset':o} for m,o in modules[:8]],
                           'pid_bound':side=='client' and game_pid==pid,'phase':'exit_logs_requires_timing_review'})
    runtime=directory/'remix-dxvk.log'
    if runtime.is_file() and runtime.stat().st_size<=16*1024*1024:
        numbers=re.findall(r'\[(\d+)\] common device objects were not disposed of',runtime.read_text(errors='replace'));remaining=max(map(int,numbers),default=0)
    status='abnormal' if faults else 'unverified' if unbound or len(cleanup)!=2 else 'cleanup_warning' if remaining else 'clean_reported'
    return {'schema':1,'status':status,'exceptions':faults,'unbound_exception_logs':unbound,'cleanup_logs':sorted(cleanup),
            'backend_objects_remaining':remaining,'limits':'Log evidence only. Cleanup messages alone do not prove a clean process exit; named stack frames do not establish the unique culprit.'}


def finish_capture(directory,root,pid,report,notify):
    # The completed Draw evidence is independent of the subsequent live probe.
    # Always persist its analysis, including when the process exits meanwhile.
    traces={}
    for path in sorted(directory.glob('rt-draw-host-*.bin')):
        try:
            traces[path.name]=analyze(path)
        except Exception as error:
            traces[path.name]={'pass':False,'errors':[f'{type(error).__name__}: {error}']}
            report['errors'].append('Draw analysis failed: '+path.name+': '+str(error))
    save(directory/'draw-analysis.json',traces)
    report['draw_observation']='pass' if traces and all(t['pass'] for t in traces.values()) else 'fail_or_partial'
    notify('Draw observation: '+report['draw_observation']+'. Reading module and queue ABI evidence...')
    binding={'execution':'incomplete'}
    try:
        if not pid:raise ValueError('No game process available for the live module probe')
        binding=collect(pid,json.loads((root/'research/rt-source-audit.json').read_text(encoding='utf-8')))
        if binding.get('execution')!='snapshot_collected':
            report['errors'].append('Live module evidence incomplete; completed Draw analysis retained')
    except Exception as error:
        binding['error']=f'{type(error).__name__}: {error}'
        report['errors'].append('Live module probe failed: '+binding['error'])
        notify(report['errors'][-1])
    save(directory/'game-binding.json',binding)
    report['module_evidence']=binding['execution']
    report['collection']='complete' if report['draw_observation']=='pass' and binding['execution']=='snapshot_collected' else 'incomplete'
    report['result_scope']='consumer state / RPC ordering only; no Source producer queue or RT image acceptance'


def finish_world_capture(directory,game,since,report,notify,beta=False,performance=False,pilot_revision=0):
    # A Host may have exited while the game remains alive. Retain its errors
    # before the potentially long normal-exit wait, including in manual copies
    # of the result directory. The final pass refreshes these logs after exit.
    try:
        collect_game_logs(game,directory,since)
        report['pre_exit_game_logs']='snapshot_collected'
    except OSError as error:
        report['pre_exit_game_logs']='incomplete'
        report['errors'].append('Pre-exit game log collection: '+str(error))
    checkpoint=dict(report,collection='incomplete',collection_phase='waiting_for_game_and_host_exit')
    save(directory/'summary.json',checkpoint)
    notify(('Capture segment and live module probe finished.' if report['collection']=='complete' else
            'INCOMPLETE capture; available trace and live module probe saved.')+
           ' Incomplete summary and available logs saved; final logs will join this ONE ZIP after L4D2 exits.')
    notify('Saving recovery ZIP beside the result folder; this checkpoint remains INCOMPLETE until final analysis.')
    recovery=archive_result(directory,checkpoint)
    notify('Recovery ZIP saved: '+str(recovery)+'. Exit L4D2 normally and keep this window open; the SAME ZIP will be updated after exit and analysis.')
    wait_stopped(notify)
    notify('Game and Host exited. Analyzing readbacks and textures, then updating the final ZIP...')
    if performance:
        performance_review=review_performance(directory);save(directory/'performance-analysis.json',performance_review)
        report['performance']='arc_model_resource_instance_collected' if performance_review['pass'] and pilot_revision>=47 else 'arc_persistent_mesh_update_collected' if performance_review['pass'] and pilot_revision>=44 else 'arc_structural_frame_batch_collected' if performance_review['pass'] else 'incomplete'
        report['preview']='completed_performance_lifecycle' if performance_review['pass'] else 'incomplete'
        if not performance_review['pass']:report['errors'].extend(performance_review['errors']);report['collection']='incomplete'
    elif report.get('preview'):
        preview=review_preview(directory);save(directory/'preview-analysis.json',preview)
        report['preview']='completed_requires_image_review' if preview['pass'] else 'incomplete'
        if not preview['pass']:report['errors'].extend(preview['errors']);report['collection']='incomplete'
    runtime=analyze_runtime(directory);save(directory/'runtime-analysis.json',runtime)
    report['runtime_profile']=runtime['status']
    if not runtime['pass']:report['errors'].extend(runtime['errors']);report['collection']='incomplete'
    if beta:
        report['beta_session']='completed_requires_image_review' if report.get('preview')=='completed_requires_image_review' and runtime['pass'] else 'incomplete'
        report['result_scope']='five-minute complete-coverage gameplay beta; visual correctness and long-term stability still require review'
        return
    if performance:
        report['result_scope']='Arc model resource/instance feasibility: multi-surface bind-pose resources, one GPU-skinned instance per Source scope, zero CPU-pose/frame-batch fallback, complete RT resource retirement and 60 surviving raster Presents; visual completeness and throughput require review' if pilot_revision>=47 else 'Arc complete explicit-model balance: optimized CPU-pose fallback with GPU-skin limits 64/80/96; global vertex capture disabled; complete RT resource retirement and 60 surviving raster Presents; visual completeness requires review'
        return
    from analyze_rt_infected import analyze as analyze_infected
    infected=analyze_infected(directory);save(directory/'infected-analysis.json',infected)
    report['infected_evidence']=infected['status']
    if not infected['pass']:report['collection']='incomplete';report['errors'].extend(infected['errors'])
    outputs=analyze_world(directory);save(directory/'world-analysis.json',outputs)
    report['world_outputs']=outputs['runtime_outputs']
    live=outputs.get('live_mesh',{})
    textures=live.get('textures',{})
    report['model_texture_coverage']=textures.get('coverage','unrun')
    report['model_texture_fallback_submissions']=textures.get('fallback_submissions',0)
    report['model_budget_omissions']=live.get('budget_omissions',0)
    report['model_budget_coverage']=live.get('model_budget_coverage','unrun')
    if report['model_budget_omissions']:
        notify('Model budget omissions: '+str(report['model_budget_omissions'])+'. Collection continues; model coverage is PARTIAL and requires review.')
    report['world_geometry_buffers']='nonzero_requires_review' if outputs.get('nonzero_world_depth_and_normals') else 'incomplete'
    reviews=json.loads((directory/'draw-analysis.json').read_text())
    report['world_converted_draws']=sum(t.get('world_conversion',{}).get('converted',0) for t in reviews.values())
    for kind in ('sky','rigid','bulk_static'):
        report[kind+'_converted_draws']=sum(t.get('world_conversion',{}).get(kind+'_converted',0) for t in reviews.values())
    inventories={name:t.get('static_family_inventory',{}) for name,t in reviews.items()}
    save(directory/'static-coverage.json',inventories)
    report['static_inventory_omitted_draws']=sum(t.get('omitted_draws',0) for t in inventories.values())
    notify('Additional static conversions: '+str(report['bulk_static_converted_draws'])+'. Whole-family rejection counts are in static-coverage.json; counts do not prove image coverage.')
    scene=analyze_scene(directory);save(directory/'scene-analysis.json',scene)
    report['model_rebuild_result']=scene.get('model_rebuild_result','not_requested');report['scene_input_evidence']=scene['status'];report['sky_rebuild_result']=scene['sky_rebuild_result'] if 'sky_rebuild_result' in scene else 'not_requested'
    if not scene['pass']:report['collection']='incomplete'
    if scene.get('sky_rebuild_summaries'):
        counts=scene['sky_rebuild_summaries'][-1]
        notify('Sky reconstruction: '+str(counts.get('reconstructed',0))+'/'+str(counts.get('attempts',0))+'; v8 fallback: '+str(counts.get('fallback',0))+'. '+scene['sky_rebuild_result']+'.')
    report['rt_activation_requests']=outputs.get('rt_activation_requests',0)
    notify('World conversion: '+str(report['world_converted_draws'])+' Draws; RT enable requests: '+
           str(report['rt_activation_requests'])+'. Actual RT images still require review.')
    report['world_normalization']='pass' if reviews and all(t.get('world_normalization')=='pass' for t in reviews.values()) else 'incomplete'
    if (outputs['runtime_outputs']!='pass' or report['world_normalization']!='pass'
            or report['world_geometry_buffers']=='incomplete'):report['collection']='incomplete'


def review_preview(directory):
    directory=Path(directory);errors=[];path=directory/'preview-runtime.jsonl'
    if not path.is_file() or path.stat().st_size>32*1024*1024:return {'pass':False,'errors':['missing_or_oversized_preview_log']}
    events=[json.loads(s) for s in path.read_text(encoding='utf-8').splitlines() if s.strip()]
    v23=any(e.get('event')=='world_coverage_revision' and e.get('value')=='23' for e in events)
    ends=[e for e in events if e.get('event')=='preview_end'];reads=[e for e in events if e.get('event')=='preview_readback']
    if len(ends)!=1 or not ends[0].get('complete') or ends[0].get('elapsed_ms',0)<60000 or ends[0].get('submitted',0)<=0 or any(ends[0].get(k)!=0 for k in ('entries','pending_fences','held')):errors.append('preview_duration_or_completion')
    if len(reads)!=3:errors.append('preview_readback_count')
    read_files=set();metrics=[]
    for e in reads:
        name=e.get('file','');file=directory/name
        if name not in ('preview-0.rgba32f','preview-1.rgba32f','preview-2.rgba32f') or name in read_files or e.get('copy_code')!=0 or e.get('hresult',0)&0x80000000 or not e.get('written') or not file.is_file() or file.stat().st_size!=640*400*16:errors.append('preview_readback_payload')
        read_files.add(name)
        if file.is_file() and file.stat().st_size==640*400*16 and name in ('preview-0.rgba32f','preview-1.rgba32f','preview-2.rgba32f'):
            raw=file.read_bytes();values=array('f');values.frombytes(raw)
            if sys.byteorder!='little':values.byteswap()
            invalid=sum(not math.isfinite(v) for i,v in enumerate(values) if i%4!=3)
            metrics.append({'file':name,'sha256':hashlib.sha256(raw).hexdigest(),'nonfinite_rgb':invalid})
            if invalid:errors.append('preview_nonfinite_rgb')
    for e in events:
        if e.get('code',0):errors.append('preview_runtime_failure:'+e.get('event','unknown'))
        if e.get('event')=='api_mesh_frame' and (e.get('bytes',0)>(512 if v23 else 256)*1024*1024 or e.get('texture_resident_bytes',0)>128*1024*1024 or e.get('texture_bytes',0)>(4096 if v23 else 512)*1024*1024 or e.get('pending_fences',0)>8):errors.append('preview_budget')
    return {'schema':1,'pass':not errors,'errors':sorted(set(errors)),'end':ends[-1] if ends else None,'color_metrics':metrics,'readbacks':reads,'gpu_image_acceptance':'unrun','limits':'Bounded neutral realtime preview, independent of600-Present diagnostic; no sustained performance or full coverage acceptance.'}


def review_performance(directory):
    directory=Path(directory);errors=[];path=directory/'preview-runtime.jsonl'
    if not path.is_file() or path.stat().st_size>32*1024*1024:return {'schema':1,'pass':False,'errors':['missing_or_oversized_performance_log']}
    events=[json.loads(s) for s in path.read_text(encoding='utf-8').splitlines() if s.strip()]
    stability=any(e.get('event')=='performance_stability_begin' for e in events)
    lifecycle=any(e.get('event')=='performance_lifecycle_begin' for e in events)
    revision=max((int(e.get('value')) for e in events if e.get('event')=='pilot_delivery_revision' and str(e.get('value','')).isdigit()),default=0)
    custom_profile=revision>=36
    native_fallback=revision==38
    complete_fallback=39<=revision<47
    resource_instance=revision>=47
    structural_batch=43<=revision<47
    persistent_batch=44<=revision<47
    ends=[e for e in events if e.get('event')=='preview_end']
    minimum_frames=(150 if revision>=39 else 270) if lifecycle else 128 if stability else 120
    if len(ends)!=1 or not ends[0].get('complete') or ends[0].get('frames',0)<minimum_frames or any(ends[0].get(k)!=0 for k in ('entries','pending_fences','held')):errors.append('performance_completion')
    elif complete_fallback and not structural_batch:
        if ends[0].get('cpu_pose_submitted',0)<=0 or ends[0].get('cpu_pose_created',-1)+ends[0].get('cpu_pose_reused',-1)!=ends[0].get('cpu_pose_submitted') or ends[0].get('submitted')!=ends[0].get('skinned_submitted',0)+ends[0].get('cpu_pose_submitted',0) or ends[0].get('budget_omitted')!=0:errors.append('performance_complete_cpu_pose_fallback')
    elif not complete_fallback and ends[0].get('submitted')!=ends[0].get('skinned_submitted'):errors.append('performance_cpu_pose_submission')
    if lifecycle:
        recovery=[e for e in events if e.get('event')=='performance_resources_recovered' and e.get('code')==0]
        raster=[e for e in events if e.get('event')=='performance_raster_survival_complete' and e.get('code')==0]
        if len(recovery)!=1 or len(raster)!=1 or len(ends)!=1 or ends[0].get('rt_frames')!=(90 if revision>=39 else 210) or ends[0].get('raster_survival_frames',0)<60 or ends[0].get('resources_recovered') is not True:
            errors.append('performance_lifecycle_recovery')
        if native_fallback and (not isinstance(ends[0].get('original_fallback_draws'),int) or ends[0]['original_fallback_draws']<=0):errors.append('performance_original_fallback_not_exercised')
    for e in events:
        if e.get('code',0):errors.append('performance_runtime_failure:'+e.get('event','unknown'))
    remix_mode={'importance_sampled_activated':False,'restir_gi_activated':False,'graphics_preset_custom_read':False,'lines':[]}
    remix_log=directory/'remix-dxvk.log'
    if custom_profile:
        config_events=list(events);world_log=directory/'world-runtime.jsonl'
        if world_log.is_file() and world_log.stat().st_size<=32*1024*1024:
            try:config_events.extend(json.loads(line) for line in world_log.read_text(encoding='utf-8').splitlines() if line.strip())
            except json.JSONDecodeError:errors.append('invalid_world_runtime_config_log')
        configured=any(e.get('event')=='rtx.graphicsPreset' and e.get('code')==0 and e.get('value')=='4' for e in config_events)
        indirect=any(e.get('event')=='rtx.integrateIndirectMode' and e.get('code')==0 and e.get('value')==('1' if lifecycle else '0') for e in config_events)
        vertex_capture=any(e.get('event')=='rtx.useVertexCapture' and e.get('code')==0 and e.get('value')=='True' for e in config_events)
        if not configured:errors.append('custom_graphics_preset_not_requested')
        if not indirect:errors.append('explicit_indirect_mode_not_requested')
        if native_fallback and not vertex_capture:errors.append('native_vertex_capture_fallback_not_requested')
        if revision>=39 and vertex_capture:errors.append('global_vertex_capture_not_disabled')
        if not remix_log.is_file() or remix_log.stat().st_size>64*1024*1024:errors.append('missing_or_oversized_remix_log')
        else:
            for line in remix_log.read_text(encoding='utf-8',errors='replace').splitlines():
                if 'Integrate Indirect Mode:' in line or 'rtx.graphicsPreset' in line:
                    remix_mode['lines'].append(line[-512:])
                if 'Integrate Indirect Mode: Importance Sampled - activated' in line:remix_mode['importance_sampled_activated']=True
                if 'Integrate Indirect Mode: ReSTIR GI - activated' in line:remix_mode['restir_gi_activated']=True
                if 'rtx.graphicsPreset = 4' in line:remix_mode['graphics_preset_custom_read']=True
                if 'Skipping draw call with shader usage as vertex capture is not enabled.' in line:remix_mode['vertex_capture_disabled_skip']=True
            if lifecycle:
                if not remix_mode['restir_gi_activated'] or remix_mode['importance_sampled_activated']:errors.append('effective_indirect_mode_not_restir_gi')
                if native_fallback and remix_mode.get('vertex_capture_disabled_skip'):errors.append('native_vertex_capture_fallback_not_effective')
            elif not remix_mode['importance_sampled_activated'] or remix_mode['restir_gi_activated']:errors.append('effective_indirect_mode_not_importance_sampled')
    if resource_instance:
        mesh=[e for e in events if e.get('event')=='api_mesh_frame' and e.get('gpu_skin_performance_phase',-1)>=0]
        scopes=[e for e in events if e.get('event')=='model_resource_scope_submitted']
        end=ends[0] if len(ends)==1 else {}
        keys=('model_resource_scopes','model_resource_definitions','model_resource_instances','model_resource_failures')
        valid=(len(mesh)==3 and [e.get('gpu_skin_performance_phase') for e in mesh]==[0,1,2] and
            all(all(isinstance(e.get(k),int) and e[k]>=0 for k in keys) for e in mesh+[end]) and
            all(e.get('model_resource_failures')==0 for e in mesh+[end]) and
            all(e.get('model_resource_scopes')==e.get('model_resource_instances') for e in mesh+[end]) and
            all(mesh[i].get('model_resource_instances',0)>(mesh[i-1].get('model_resource_instances',0) if i else 0) for i in range(3)) and
            0<end.get('model_resource_definitions',0)<end.get('model_resource_instances',0) and
            end.get('model_resource_instances')==len(scopes) and sum(e.get('created') is True for e in scopes)==end.get('model_resource_definitions') and
            all(isinstance(e.get('frame'),int) and 0<e['frame']<=90 and isinstance(e.get('surfaces'),int) and e['surfaces']>0 and
                isinstance(e.get('bones'),int) and e['bones']>0 for e in scopes) and
            not any(e.get('event')=='frame_batch_flush' for e in events) and
            all(end.get(k)==0 for k in ('cpu_pose_submitted','cpu_pose_created','cpu_pose_reused','frame_batch_flushes','frame_batch_source_draws',
                'frame_batch_submitted_source_draws','frame_batch_failures','frame_batch_worker_submitted','frame_batch_worker_completed',
                'frame_batch_worker_failed','frame_batch_worker_threads','frame_batch_worker_max_active')) and
            end.get('submitted')==end.get('skinned_submitted')==end.get('model_resource_instances') and
            end.get('skinned_created',-1)+end.get('skinned_reused',-1)==end.get('skinned_submitted'))
        if not valid:errors.append('performance_model_resource_instance')
        windows={e.get('segment'):e for e in events if e.get('event')=='runtime_timing_window'};phases=[]
        prior_instances=0
        for index,name in enumerate(('model_resource_warm','model_resource_steady','model_resource_reuse')):
            row=windows.get(name);wall=row.get('wall_ms',0) if row else 0;evidence=mesh[index] if len(mesh)>index else {}
            if not isinstance(wall,(int,float)) or not math.isfinite(wall) or wall<=0:errors.append('performance_timing:'+name);continue
            instances=evidence.get('model_resource_instances',prior_instances)
            if not isinstance(instances,int) or instances<=prior_instances:errors.append('performance_model_resource_progress:'+name);instances=prior_instances
            thread_cpu=row.get('thread_cpu_ms')
            if row.get('thread_times_available') is not True or not isinstance(thread_cpu,(int,float)) or not math.isfinite(thread_cpu) or not 0<=thread_cpu<=wall+1:
                errors.append('performance_thread_cpu_timing:'+name)
            phases.append({'name':name,'frames':30,'wall_ms':wall,'fps':30000/wall,
                'model_resource_instances_delta':instances-prior_instances,
                'model_resource_definitions':evidence.get('model_resource_definitions'),
                'model_resource_reuse':instances-evidence.get('model_resource_definitions',instances),
                'host_device_thread_cpu_ms':thread_cpu if isinstance(thread_cpu,(int,float)) else None,
                'host_device_thread_non_cpu_ms':max(0,wall-thread_cpu) if isinstance(thread_cpu,(int,float)) else None,
                'model_ms':row.get('scopes',{}).get('model',{}).get('elapsed_ms',0),
                'capture_ms':row.get('scopes',{}).get('prepare',{}).get('elapsed_ms',0),
                'mesh_create_ms':row.get('scopes',{}).get('mesh_create',{}).get('elapsed_ms',0),
                'instance_ms':row.get('scopes',{}).get('instance',{}).get('elapsed_ms',0)})
            prior_instances=instances
        raster_window=windows.get('gpu_lifecycle_raster')
        if not raster_window or not isinstance(raster_window.get('wall_ms'),(int,float)) or not math.isfinite(raster_window['wall_ms']) or raster_window['wall_ms']<=0:errors.append('performance_raster_timing')
        best=max((p['fps'] for p in phases),default=0);worst=min((p['fps'] for p in phases),default=0)
        return {'schema':1,'pass':not errors,'errors':sorted(set(errors)),'profile':'arc_model_resource_instance','phases':phases,
            'best_fps':best,'worst_fps':worst,'meets_24_fps':bool(phases) and worst>=24,
            'model_resources':{k:end.get(k) for k in keys},'effective_indirect_mode':remix_mode,
            'raster_survival':{'frames':end.get('raster_survival_frames',0),'timing':raster_window},'end':end,
            'limits':'v47 defines each multi-surface bind-pose model resource once, submits one GPU-skinned instance per Source model scope before Present, and rejects any CPU-pose or frame-batch fallback. Visual completeness and throughput still require user review.'}
    if structural_batch:
        mesh=[e for e in events if e.get('event')=='api_mesh_frame' and e.get('gpu_skin_performance_phase',-1)>=0]
        if [e.get('gpu_skin_performance_phase') for e in mesh]!=[0,1,2] or [e.get('skinned_frame_limit') for e in mesh]!=[80,0,0]:
            errors.append('performance_phase_sequence')
        elif (mesh[0].get('skinned_frame_max')!=80 or not isinstance(mesh[0].get('skinned_frame_submitted'),int) or
              not 0<mesh[0]['skinned_frame_submitted']<=80 or mesh[0].get('skinned_frame_unique_meshes')!=mesh[0]['skinned_frame_submitted'] or
              mesh[0].get('skinned_frame_repeat_draws')!=0 or any(e.get('skinned_frame_max')!=80 or e.get('skinned_frame_submitted')!=0 for e in mesh[1:])):
            errors.append('performance_explicit_oracle_not_exercised')
        flushes=[e for e in events if e.get('event')=='frame_batch_flush']
        valid_flushes=len(flushes)==60 and all(
            all(isinstance(e.get(k),int) and e[k]>=0 for k in ('source_draws','source_vertices','source_indices','batches','submitted_source_draws','worker_submitted','worker_completed','worker_failed','worker_threads','worker_max_active')) and
            e['source_draws']>0 and e['source_vertices']>0 and e['source_indices']>0 and e['batches']>0 and
            e['submitted_source_draws']==e['source_draws'] and e['worker_failed']==0 and e['worker_threads']==4 and
            isinstance(e.get('worker_cpu_elapsed_ms'),(int,float)) and math.isfinite(e['worker_cpu_elapsed_ms']) and e['worker_cpu_elapsed_ms']>=0
            for e in flushes)
        if not valid_flushes:errors.append('performance_frame_batch_evidence')
        if persistent_batch and valid_flushes:
            warm,persistent=flushes[:30],flushes[30:]
            persistent_valid=(all(e.get('persistent_phase') is False for e in warm) and
                all(e.get('persistent_phase') is True and isinstance(e.get('persistent_created'),int) and
                    isinstance(e.get('persistent_updated'),int) and e['persistent_created']>=0 and e['persistent_updated']>=0 for e in persistent) and
                bool(persistent) and persistent[0]['persistent_created']>0 and persistent[0]['persistent_updated']>=0 and
                all(e['persistent_created']==0 and e['persistent_updated']>0 for e in persistent[1:]) and len(ends)==1 and
                ends[0].get('persistent_mesh_updates')==sum(e['persistent_updated'] for e in persistent))
            if not persistent_valid:errors.append('performance_persistent_mesh_update')
        total_source=sum(e.get('source_draws',0) for e in flushes);total_batches=sum(e.get('batches',0) for e in flushes)
        end=ends[0] if len(ends)==1 else {}
        if (not valid_flushes or total_batches>=total_source or max((e.get('worker_max_active',0) for e in flushes),default=0)<2 or
            end.get('frame_batch_flushes')!=60 or end.get('frame_batch_source_draws')!=total_source or
            end.get('frame_batch_submitted_source_draws')!=total_source or end.get('frame_batch_failures')!=0 or
            end.get('frame_batch_worker_submitted')!=total_source or end.get('frame_batch_worker_completed')!=total_source or
            end.get('frame_batch_worker_failed')!=0 or end.get('frame_batch_worker_threads')!=4 or end.get('frame_batch_worker_max_active',0)<2):
            errors.append('performance_frame_batch_accounting')
        windows={e.get('segment'):e for e in events if e.get('event')=='runtime_timing_window'}
        gpu_rows=[];gpu_file=directory/'gpu-engine-samples.jsonl'
        if gpu_file.is_file() and gpu_file.stat().st_size<=32*1024*1024:
            try:gpu_rows=[json.loads(line) for line in gpu_file.read_text(encoding='utf-8').splitlines() if line.strip()]
            except json.JSONDecodeError:gpu_rows=[]
        names=('explicit_oracle_80','frame_batch_warm','persistent_batch_update' if persistent_batch else 'frame_batch_steady');phases=[]
        prior_submitted=0
        for index,name in enumerate(names):
            row=windows.get(name);wall=row.get('wall_ms',0) if row else 0
            if not isinstance(wall,(int,float)) or not math.isfinite(wall) or wall<=0:
                errors.append('performance_timing:'+name);continue
            evidence=mesh[index] if len(mesh)>index else {};submitted=evidence.get('preview_submitted',-1)
            if not isinstance(submitted,int) or submitted<prior_submitted:errors.append('performance_batch_api_accounting:'+name);submitted=prior_submitted
            api_delta=submitted-prior_submitted
            phase_flushes=[] if index==0 else flushes[:30] if index==1 else flushes[30:]
            phase_source=sum(e.get('source_draws',0) for e in phase_flushes);phase_batches=sum(e.get('batches',0) for e in phase_flushes)
            if index and api_delta!=phase_batches:errors.append('performance_batch_api_accounting:'+name)
            phase={'name':name,'gpu_skin_limit':80 if index==0 else 0,'frames':30,'wall_ms':wall,'fps':30000/wall,
                   'api_mesh_submitted_delta':api_delta,'source_draws':phase_source,'api_batches':phase_batches,
                   'batch_compression_ratio':phase_source/phase_batches if phase_batches else None,
                   'model_ms':row.get('scopes',{}).get('model',{}).get('elapsed_ms',0),
                   'capture_ms':row.get('scopes',{}).get('prepare',{}).get('elapsed_ms',0),
                   'batch_wait_ms':row.get('scopes',{}).get('batch_wait',{}).get('elapsed_ms',0),
                   'batch_assemble_ms':row.get('scopes',{}).get('batch_assemble',{}).get('elapsed_ms',0),
                   'batch_submit_ms':row.get('scopes',{}).get('batch_submit',{}).get('elapsed_ms',0)}
            thread_cpu=row.get('thread_cpu_ms');
            if row.get('thread_times_available') is not True or not isinstance(thread_cpu,(int,float)) or not math.isfinite(thread_cpu) or not 0<=thread_cpu<=wall+1:
                errors.append('performance_thread_cpu_timing:'+name)
            else:phase['host_device_thread_cpu_ms']=thread_cpu;phase['host_device_thread_non_cpu_ms']=max(0,wall-thread_cpu)
            if isinstance(row.get('utc_filetime_100ns'),int):
                end_ns=(row['utc_filetime_100ns']-116444736000000000)*100;start_ns=end_ns-int(wall*1000000);values=[]
                for sample in gpu_rows:
                    if start_ns<=sample.get('unix_ns',0)<=end_ns:
                        engines=[e.get('percent',0) for e in sample.get('engines',[]) if e.get('instance','').endswith('engtype_3D') and isinstance(e.get('percent'),(int,float)) and math.isfinite(e['percent'])]
                        if engines:values.append(max(engines))
                if values:
                    ordered=sorted(values);phase['gpu_3d_engine']={'samples':len(values),'average_percent':sum(values)/len(values),'p50_percent':ordered[len(ordered)//2],'p95_percent':ordered[int((len(ordered)-1)*.95)],'max_percent':max(values)}
            phases.append(phase);prior_submitted=submitted
        raster_window=windows.get('gpu_lifecycle_raster')
        if not raster_window or not isinstance(raster_window.get('wall_ms'),(int,float)) or not math.isfinite(raster_window['wall_ms']) or raster_window['wall_ms']<=0:errors.append('performance_raster_timing')
        best=max((p['fps'] for p in phases),default=0);worst=min((p['fps'] for p in phases),default=0)
        persistent_comparison=None
        if persistent_batch and len(phases)==3 and phases[1]['fps']>0:
            warm,persistent=phases[1],phases[2]
            persistent_comparison={
                'control':'frame_batch_warm','candidate':'persistent_batch_update',
                'fps_gain_percent':(persistent['fps']/warm['fps']-1)*100,
                'wall_ms_change':persistent['wall_ms']-warm['wall_ms'],
                'batch_assemble_ms_change':persistent['batch_assemble_ms']-warm['batch_assemble_ms'],
                'batch_submit_ms_change':persistent['batch_submit_ms']-warm['batch_submit_ms'],
                'material_gain_threshold_percent':10,
                'material_gain':persistent['fps']>=warm['fps']*1.10}
        return {'schema':1,'pass':not errors,'errors':sorted(set(errors)),'profile':'arc_persistent_mesh_update' if persistent_batch else 'arc_structural_frame_batch','phases':phases,
                'best_fps':best,'worst_fps':worst,'meets_24_fps':bool(phases) and worst>=24,
                'persistent_vs_warm':persistent_comparison,
                'frame_batch':{'flushes':len(flushes),'source_draws':total_source,'api_batches':total_batches,
                               'compression_ratio':total_source/total_batches if total_batches else None,
                               'worker_max_active':max((e.get('worker_max_active',0) for e in flushes),default=0)},
                'effective_indirect_mode':remix_mode,'raster_survival':{'frames':end.get('raster_survival_frames',0),'timing':raster_window},
                'end':end,'limits':('v46 preserves GPU-80 and frame-batch controls, keeps expected unsupported model passes as per-Draw fallbacks, and gives dynamic CPU-pose batches stable Runtime topology identity from their first upsert so vertex changes use BVH update/refit. Runtime input buffers are still allocated per update.' if persistent_batch else 'v43 preserves a 30-frame GPU-80 explicit oracle, then moves CPU pose reconstruction to four Host workers and submits 30 warm plus 30 steady frames as material/transform batches. Source-draw completion and API-batch compression are audited independently; visual completeness still requires user review.')}
    mesh=[e for e in events if e.get('event')=='api_mesh_frame' and e.get('gpu_skin_performance_phase',-1)>=0]
    expected=([64,80,96] if revision>=42 else [64,64,64] if revision==41 else [16,64,128] if revision==40 else [0,8,16]) if lifecycle else [32]*4 if stability else [16,32,48,64]
    if [e.get('gpu_skin_performance_phase') for e in mesh]!=list(range(len(expected))) or [e.get('skinned_frame_limit') for e in mesh]!=expected:
        errors.append('performance_phase_sequence')
    elif any(not isinstance(e.get('skinned_frame_submitted'),int) or not 0<=e['skinned_frame_submitted']<=e['skinned_frame_limit'] or
             not isinstance(e.get('skinned_frame_unique_meshes'),int) or not isinstance(e.get('skinned_frame_repeat_draws'),int) or
             e['skinned_frame_unique_meshes']!=e['skinned_frame_submitted'] or e['skinned_frame_repeat_draws']!=0 or
             e.get('skinned_frame_max')!=((96 if revision>=42 else 64 if revision==41 else 128 if revision==40 else 16) if lifecycle else 32 if stability else e.get('skinned_frame_max')) or
             not isinstance(e.get('preview_submitted'),int) or
             (e['preview_submitted']!=e.get('preview_skinned_submitted') if not complete_fallback else
              not all(isinstance(e.get(k),int) and e[k]>=0 for k in ('preview_cpu_pose_submitted','preview_cpu_pose_created','preview_cpu_pose_reused')) or
              e['preview_cpu_pose_submitted']<=0 or e['preview_cpu_pose_created']+e['preview_cpu_pose_reused']!=e['preview_cpu_pose_submitted'] or
              e['preview_submitted']!=e.get('preview_skinned_submitted',0)+e['preview_cpu_pose_submitted']) for e in mesh):
        errors.append('performance_gpu_skin_not_exercised')
    windows={e.get('segment'):e for e in events if e.get('event')=='runtime_timing_window'};phases=[]
    warmup=[e for e in events if e.get('event')=='api_mesh_frame' and e.get('gpu_skin_performance_phase')==-1 and e.get('preview_submitted',0)>0]
    previous=dict(warmup[-1]) if stability and warmup else {}
    # v33 executed the correct 48/64 limits but inherited v32's final two
    # timing labels. Prefer v34 labels while preserving archived v33 evidence.
    legacy_timing='gpu_ramp_48' not in windows and 'gpu_ramp_128' in windows
    gpu_rows=[];gpu_file=directory/'gpu-engine-samples.jsonl'
    if gpu_file.is_file() and gpu_file.stat().st_size<=32*1024*1024:
        try:gpu_rows=[json.loads(line) for line in gpu_file.read_text(encoding='utf-8').splitlines() if line.strip()]
        except json.JSONDecodeError:gpu_rows=[]
    phase_frames=([30,30,30] if complete_fallback else [30,60,120]) if lifecycle else [30]*len(expected)
    lifecycle_names=(['gpu_optimized_64','gpu_optimized_80','gpu_optimized_96'] if revision>=42 else
                     ['cpu_pose_fast_1','cpu_pose_fast_2','cpu_pose_fast_3'] if revision==41 else
                     ['gpu_lifecycle_16','gpu_lifecycle_64','gpu_lifecycle_128'] if revision==40 else
                     ['gpu_lifecycle_cpu_complete' if complete_fallback else 'gpu_lifecycle_native_fallback' if native_fallback else 'gpu_lifecycle_static','gpu_lifecycle_8','gpu_lifecycle_16'])
    for index,limit in enumerate(expected):
        name=lifecycle_names[index] if lifecycle else 'gpu_stability_'+str(index+1) if stability else 'gpu_ramp_'+str(limit)
        timing_name=name if stability or lifecycle else ('gpu_ramp_64' if limit==48 else 'gpu_ramp_128' if limit==64 else name) if legacy_timing else name
        row=windows.get(timing_name);wall=row.get('wall_ms',0) if row else 0
        if not isinstance(wall,(int,float)) or not math.isfinite(wall) or wall<=0:errors.append('performance_timing:'+name);continue
        evidence=mesh[index] if len(mesh)>index else {};current={k:evidence.get(k) for k in ('preview_skinned_submitted','preview_skinned_created','preview_skinned_reused')}
        if not all(isinstance(value,int) and value>=previous.get(key,0) for key,value in current.items()):errors.append('performance_gpu_skin_accounting:'+name)
        submitted_delta=current.get('preview_skinned_submitted',0)-previous.get('preview_skinned_submitted',0)
        created_delta=current.get('preview_skinned_created',0)-previous.get('preview_skinned_created',0)
        reused_delta=current.get('preview_skinned_reused',0)-previous.get('preview_skinned_reused',0)
        cpu_pose_delta=(evidence.get('preview_submitted',0)-evidence.get('preview_skinned_submitted',0))-(previous.get('preview_submitted',0)-previous.get('preview_skinned_submitted',0))
        zero_baseline=index==0 and lifecycle and revision<40
        if (zero_baseline and (submitted_delta!=0 or created_delta!=0 or reused_delta!=0)) or (not zero_baseline and submitted_delta<=0) or created_delta+reused_delta!=submitted_delta:errors.append('performance_gpu_skin_not_exercised:'+name)
        if complete_fallback and cpu_pose_delta<=0:errors.append('performance_cpu_pose_fallback_not_exercised:'+name)
        phase={'gpu_skin_limit':limit,'frames':phase_frames[index],'wall_ms':wall,'fps':phase_frames[index]*1000/wall,
                       'skinned_frame_submitted':evidence.get('skinned_frame_submitted',0),
                       'skinned_frame_unique_meshes':evidence.get('skinned_frame_unique_meshes',0),
                       'skinned_frame_repeat_draws':evidence.get('skinned_frame_repeat_draws',0),
                       'gpu_skin_submitted_delta':submitted_delta,
                       'gpu_skin_created_delta':created_delta,
                       'gpu_skin_reused_delta':reused_delta,
                       'cpu_pose_submitted_delta':cpu_pose_delta,
                       'mesh_created_delta':evidence.get('created',0)-previous.get('created',0),
                       'mesh_reused_delta':evidence.get('reused',0)-previous.get('reused',0),
                       'model_ms':row.get('scopes',{}).get('model',{}).get('elapsed_ms',0),
                       'cpu_skin_ms':row.get('scopes',{}).get('skin',{}).get('elapsed_ms',0),
                       'mesh_create_ms':row.get('scopes',{}).get('mesh_create',{}).get('elapsed_ms',0),
                       'present_ms':row.get('scopes',{}).get('present',{}).get('elapsed_ms',0)}
        if custom_profile:
            thread_cpu=row.get('thread_cpu_ms');thread_available=row.get('thread_times_available') is True
            if not thread_available or not isinstance(thread_cpu,(int,float)) or not math.isfinite(thread_cpu) or not 0<=thread_cpu<=wall+1:
                errors.append('performance_thread_cpu_timing:'+name)
            else:
                phase['host_device_thread_cpu_ms']=thread_cpu
                phase['host_device_thread_non_cpu_ms']=max(0,wall-thread_cpu)
        if isinstance(row.get('utc_filetime_100ns'),int):
            end_ns=(row['utc_filetime_100ns']-116444736000000000)*100;start_ns=end_ns-int(wall*1000000);values=[]
            for sample in gpu_rows:
                if start_ns<=sample.get('unix_ns',0)<=end_ns:
                    engines=[e.get('percent',0) for e in sample.get('engines',[]) if e.get('instance','').endswith('engtype_3D') and isinstance(e.get('percent'),(int,float)) and math.isfinite(e['percent'])]
                    if engines:values.append(max(engines))
            if values:
                ordered=sorted(values);phase['gpu_3d_engine']={'samples':len(values),'average_percent':sum(values)/len(values),'p50_percent':ordered[len(ordered)//2],'p95_percent':ordered[int((len(ordered)-1)*.95)],'max_percent':max(values)}
            cpu_by_pid={}
            for sample in gpu_rows:
                sample_end=sample.get('unix_ns',0)
                for cpu in sample.get('process_cpu',[]):
                    begin=cpu.get('interval_begin_unix_ns',sample_end);overlap=max(0,min(end_ns,sample_end)-max(start_ns,begin));duration=sample_end-begin
                    if not overlap or duration<=0 or not isinstance(cpu.get('cpu_ms'),(int,float)):continue
                    key=(cpu.get('pid'),cpu.get('role','unknown'));target=cpu_by_pid.setdefault(key,{'cpu_ms':0.0,'covered_wall_ms':0.0})
                    target['cpu_ms']+=cpu['cpu_ms']*overlap/duration;target['covered_wall_ms']+=overlap/1000000
            if cpu_by_pid:
                phase['process_cpu']=[{'pid':key[0],'role':key[1],**value,'one_core_percent':100*value['cpu_ms']/value['covered_wall_ms']} for key,value in sorted(cpu_by_pid.items()) if value['covered_wall_ms']>0]
        phases.append(phase);previous=dict(evidence)
    raster_window=windows.get('gpu_lifecycle_raster') if lifecycle else None
    if lifecycle and (not raster_window or not isinstance(raster_window.get('wall_ms'),(int,float)) or not math.isfinite(raster_window['wall_ms']) or raster_window['wall_ms']<=0):errors.append('performance_raster_timing')
    best=max((p['fps'] for p in phases),default=0);worst=min((p['fps'] for p in phases),default=0)
    return {'schema':1,'pass':not errors,'errors':sorted(set(errors)),'profile':'arc_restir_complete_lifecycle' if lifecycle and complete_fallback else 'arc_restir_hybrid_lifecycle' if lifecycle and native_fallback else 'arc_restir_lifecycle' if lifecycle else 'arc_direct_custom_stability' if custom_profile else 'arc_direct_stability' if stability else 'legacy_gpu_skin_ramp','phases':phases,'best_fps':best,'worst_fps':worst,'meets_24_fps':bool(phases) and worst>=24,
            'effective_indirect_mode':remix_mode if custom_profile else {'status':'not_enforced_before_v36'},
            'raster_survival':{'frames':ends[-1].get('raster_survival_frames',0) if ends else 0,'timing':raster_window},
            'end':ends[-1] if ends else None,'limits':'v42 retains the optimized complete CPU-pose fallback and compares audited persistent GPU skinning at 64/80/96 submissions per frame. Each scene runs for 30 frames before resource retirement and raster recovery; visible completeness still requires user review.' if lifecycle and revision>=42 else 'v41 fixes audited persistent GPU skinning at 64 for three 30-frame windows while optimizing complete CPU-pose fallback and raising the entry cap under the same 512 MiB byte cap. Visible completeness still requires user review.' if lifecycle and revision==41 else 'v40 keeps complete explicit CPU-pose fallback while scaling audited persistent GPU skinning through 16/64/128 submissions per frame. Each scene runs for 30 frames before resource retirement and raster recovery; visible completeness still requires user review.' if lifecycle and revision==40 else 'v39 disables global vertex capture and restores explicit CPU-pose meshes for all accepted parts outside the 0/8/16 persistent GPU-skin budget. Each complete scene runs for 30 frames before resource retirement and raster recovery; visible completeness still requires user review.' if lifecycle and complete_fallback else 'v38 keeps native vertex-captured programmable draws for unsupported and over-budget parts, and applies Remix Ignore only after an explicit replacement succeeds. The 0/8/16 explicit-instance lifecycle and raster recovery checks remain; visible completeness still requires user review.' if lifecycle and native_fallback else 'v37 uses explicit ReSTIR GI, measures 30 static RT frames, 60 frames at 8 and 120 frames at 16 GPU-skinned meshes, then requires all RT resources to retire and 60 ordinary Presents to survive. A pass is lifecycle evidence, not image acceptance.' if lifecycle else 'v36 uses the v35 fixed-32 stability load, forces the Custom preset so Arc cannot replace importance-sampled indirect mode with ReSTIR GI, and records Host device-thread plus per-process CPU time. Performance mode omits unsupported/excess CPU-pose models; image completeness is not evaluated.' if custom_profile else 'v35 uses an 8-frame staged warm-up followed by four fixed-32 30-frame windows at a fixed limit of 32 with TAAU 0.75, direct light only, secondary bounces off and RTXDI off. Performance mode omits unsupported/excess CPU-pose models; image completeness is not evaluated.' if stability else 'Sequential phases share a changing scene and are directional, not matched A/B. Performance mode omits unsupported/excess CPU-pose models from explicit RT, so image completeness is not evaluated. GPU PDH counters remain separate per engine.'}

def startup_modules(directory,pid,notify):
    """Retain bounded module lists even if no F9 trace ever starts."""
    result={'scope':'read_only_startup_module_inventory_not_capture_completion',
            'association':'game PID and discovered Host names; parent/session binding requires log review',
            'processes':[]}
    diagnostics={};game_pids(diagnostics)
    targets=[(pid,'game')]+[(p['pid'],'host_candidate') for p in diagnostics.get('related_processes',[])
                          if p['exe'].casefold()=='l4d2bridge64.exe'][:4]
    for process_id,role in targets:
        entry={'pid':process_id,'role':role};result['processes'].append(entry)
        try:
            with WindowsProcess(process_id) as process:
                entry['identity']=process.identity();entry['modules']=process.modules()[:1024]
            if role=='game':
                entry['component_files']=[]
                for module in entry['modules']:
                    if module.get('name','').casefold() not in ('necola.dll','studiorender.dll'):continue
                    file=Path(module['path']);metadata={'module':module['name'],'mapped_size':module['size']}
                    try:
                        size=file.stat().st_size
                        if size>64*1024*1024:raise ValueError('Component file exceeds metadata hash budget')
                        metadata.update(size=size,sha256=digest(file))
                    except (OSError,ValueError) as error:metadata['error']=str(error)
                    entry['component_files'].append(metadata)
        except (OSError,ValueError) as error:entry['error']=str(error)
    save(directory/'startup-modules.json',result)
    notify('Startup module inventory saved. This does not mean the F9 capture is complete.')


def run(root=ROOT,game=None,restore_only=False,verify_only=False,beta=False,performance=False):
    if verify_only:
        verify_package(root);print('Package files and collector configuration verified.');return
    if os.name!='nt':raise OSError('This launcher requires Windows x64')
    if restore_only:
        wait_stopped(print);location=restore(root);print('Original client/Host restored:',location);return
    manifest=verify_package(root)
    world=manifest.get('profile')=='world_pilot'
    if (beta or performance) and not world:raise ValueError('World runtime mode requires the world runtime package')
    if beta and performance:raise ValueError('Beta and performance modes are mutually exclusive')
    name=('rt-performance-' if performance else 'rt-beta-' if beta else 'rt-world-' if world else 'rt-draw-')+time.strftime('%Y%m%d-%H%M%S')+'-'+secrets.token_hex(4)
    directory=root/'results'/name;directory.mkdir(parents=True)
    report={'schema':1,'scope':'actual_draw_consumer_observation','build_commit':manifest['build_commit'],
            'collection':'incomplete','errors':[],'game_rt':'unrun','source_queue_execution':'unrun',
            'source_parallel_preparation':'unrun','performance':'unrun'}
    request=None;content=None;location=None;started_at=time.time();gpu_sampler=None
    def notify(message):
        print(message,flush=True)
        with (directory/'progress.log').open('a',encoding='utf-8') as f:f.write(time.strftime('%H:%M:%S ')+message+'\n')
    try:
        runtime=runtime_for_world(root,notify) if world else None
        pids=game_pids();location=locate_game(game,pids,root)
        if not (location/'left4dead2.exe').is_file():raise ValueError('Invalid L4D2 directory')
        matches=all((location/n).is_file() and digest(location/n)==manifest['binaries'][n]['sha256'] for n in TARGETS)
        if world or not matches:wait_stopped(notify)
        receipt=install(root,location,manifest);save(directory/'installation.json',receipt)
        session=secrets.randbits(63) or 1;request=location/'bin/ldb-rt-draw.request'
        content=((f'LDB_RT_PERF_1\n{session:x}\n{directory.resolve()}\n{runtime.resolve()}\n' if performance else
                  f'LDB_RT_BETA_1\n{session:x}\n{directory.resolve()}\n{runtime.resolve()}\n' if beta else
                  f'LDB_RT_WORLD_1\n{session:x}\n{directory.resolve()}\n{runtime.resolve()}\n') if world else
                 f'LDB_RT_DRAW_1\n{session:x}\n{directory.resolve()}\n').encode('utf-8')
        if world:
            config_options=['rtx.enableRaytracing = False','rtx.useVertexCapture = False','rtx.upscalerType = 0','rtx.resolutionScale = 1.0',
                'rtx.enableRayReconstruction = False','rtx.sceneScale = 0.3937007874',
                'rtx.fallbackLightMode = 2','rtx.fallbackLightType = 1','rtx.fallbackLightRadiance = 30,30,30','rtx.fallbackLightRadius = 5',
                'rtx.fallbackLightPositionOffset = 0,0,0','rtx.enableFallbackLightShaping = False','rtx.fallbackLightDirection = 0,0,-1',
                'rtx.autoExposure.enabled = True','rtx.autoExposure.evMinValue = -2','rtx.autoExposure.evMaxValue = 5',
                'rtx.tonemappingMode = 1','rtx.localtonemap.exposure = 0.75',
                'rtx.orthographicIsUI = True','rtx.skipDrawCallsPostRTXInjection = False','rtx.enableAlphaBlend = True','rtx.bloom.enable = False','rtx.postfx.enable = False']
            if performance:config_options += ['rtx.graphicsPreset = 4','rtx.upscalerType = 3','rtx.resolutionScale = 0.75','rtx.enableDirectLighting = True',
                'rtx.enableSecondaryBounces = False','rtx.useRTXDI = False','rtx.integrateIndirectMode = 1','rtx.useVertexCapture = False']
            config='\n'.join(config_options)+'\n'
            (directory/'world.conf').write_text(config,encoding='utf-8')
            report['scope']='arc_model_resource_instance_validation' if performance and manifest.get('pilot_revision',0)>=47 else 'arc_persistent_mesh_update_validation' if performance and manifest.get('pilot_revision',0)>=44 else 'arc_structural_frame_batch_validation' if performance else 'five_minute_complete_coverage_gameplay_beta' if beta else 'selected_static_world_runtime_pilot'
            report['game_rt']='attempted_requires_review'
        if request.exists():raise ValueError('Another observation request exists. Preserve it and finish its collector first: '+str(request))
        with request.open('xb') as f:f.write(content)
        notify('Enter a map, return to the game, and press F9 once. The '+('30-frame performance gate' if performance else '30-frame beta gate' if beta else '600-Present capture' if world else '120-Present capture')+' starts automatically.')
        if performance:notify('RT Performance v47: all 90 RT frames define multi-surface bind-pose model resources once and submit one GPU-skinned instance per Source model scope before Present. CPU-pose and frame-batch fallbacks are disabled. Keep several characters in view and do not press F9 again.')
        elif beta:notify('RT Beta v30: after the 30-frame safety gate, play normally for five minutes. GPU skinning keeps 16 submissions per frame at ordinary model density and a 4-submission baseline above 256 model draws; excess models use CPU poses. Do not press F9 again. Exit only when this window asks.')
        elif world:notify('World Pilot v30: keep animated characters nearby, then press F9 ONCE. Move and turn slowly. GPU skinning keeps 16 submissions per frame at ordinary model density and 4 above 256 model draws; excess models use CPU poses. Exit only when this window asks; keep it open until the final ONE ZIP path appears.')
        if not game_pids():os.startfile('steam://rungameid/550')
        done=directory/f'rt-draw-done-{session}.json'
        deadline=time.monotonic()+900;last=0;pid=None;last_alive=time.monotonic();startup_saved=False;first_alive=None
        while time.monotonic()<deadline and not done.exists():
            process_diagnostics={};candidates=game_pids(process_diagnostics)
            if gpu_sampler:gpu_sampler.track([p['pid'] for p in process_diagnostics.get('related_processes',[]) if p['exe'].casefold()=='l4d2bridge64.exe'],'host')
            if len(candidates)>1:raise ValueError('Multiple L4D2 processes found')
            if candidates:
                pid=candidates[0];last_alive=time.monotonic()
                report['game_pid']=pid
                if first_alive is None:first_alive=last_alive
                if world and manifest.get('pilot_revision',0)>=24 and gpu_sampler is None:
                    gpu_sampler=Sampler(directory).start();gpu_sampler.track([pid],'game')
                if world and not startup_saved and last_alive-first_alive>=5:
                    startup_saved=True
                    try:
                        startup_modules(directory,pid,notify)
                        if gpu_sampler:
                            modules=json.loads((directory/'startup-modules.json').read_text(encoding='utf-8'))
                            for process in modules['processes']:
                                gpu_sampler.track([process['pid']], 'game' if process.get('role')=='game' else 'host')
                    except OSError as error:notify('Startup module inventory unavailable: '+str(error))
            elif pid and time.monotonic()-last_alive>3:
                raise RuntimeError('Game exited before capture completion; partial results retained')
            if world and (directory/'world-runtime.jsonl').is_file():
                events=(directory/'world-runtime.jsonl').read_text(encoding='utf-8').splitlines()
                for line in events:
                    try:entry=json.loads(line)
                    except json.JSONDecodeError:continue
                    if entry.get('event')!='world_readback' and entry.get('code',0):
                        raise RuntimeError('World runtime failed at '+entry['event']+'; partial results retained')
            if time.monotonic()-last>=15:
                notify('Waiting for F9 / capture completion; game PID: '+str(pid));last=time.monotonic()
            time.sleep(.5)
        if not done.is_file():raise TimeoutError('No completed capture; partial logs are retained')
        marker=json.loads(done.read_text(encoding='utf-8'))
        report['capture_marker']=marker
        expected=30 if (beta or performance) else 600 if world else 120
        marker_ok=marker.get('session')==session and marker.get('reason')==0 and marker.get('presents')==expected
        if not marker_ok:
            notify('INCOMPLETE: capture stopped before the required '+str(expected)+' Presents. Collecting available evidence.')
        preview_running=directory/f'rt-preview-running-{session}.json'
        if world and manifest.get('pilot_revision',0)>=20:report['preview']='incomplete'
        if world and preview_running.is_file():
            notify('30-frame gate passed. The four-worker frame batch path is active for 60 frames. Keep characters visible and check whether their full bodies remain present; RT turns off automatically for the final raster survival check.' if performance else '30-frame safety gate passed. Five-minute RT Beta is active; play normally and watch for missing geometry, deformation, freezes or crashes.' if beta else '600-Present diagnostic saved. Live preview first30s has no AA, last30s requests native TAA. Observe edges/trails; no second keypress.')
            preview_done=directory/f'rt-preview-done-{session}.json';preview_deadline=time.monotonic()+(240 if performance else 330 if beta else 90)
            while not preview_done.is_file() and time.monotonic()<preview_deadline:
                if pid not in game_pids():break
                time.sleep(.5)
            preview_complete=completed_preview(preview_done,session)
            report['preview']='collected_requires_review' if preview_complete else 'incomplete'
            if preview_complete:notify(('Arc lifecycle run finished.' if performance else 'Five-minute Beta segment finished.' if beta else 'Preview segment finished.')+' Collecting final evidence; exit only when requested below.')
            elif preview_done.is_file():notify('Preview stopped early and retained partial evidence. Collecting final diagnostics; this run has not passed.')
        if gpu_sampler:
            report['gpu_sampler_stopped']=gpu_sampler.stop();gpu_sampler=None
        finish_capture(directory,root,pid,report,notify)
        if not marker_ok:
            report['collection']='incomplete';report['errors'].append('Capture completion marker identity/count/reason mismatch')
        if world:
            finish_world_capture(directory,location,started_at,report,notify,beta=beta,performance=performance,pilot_revision=manifest.get('pilot_revision',0))
    except Exception as error:
        report['collection']='incomplete'
        report['errors'].append(f'{type(error).__name__}: {error}');notify(report['errors'][-1])
    finally:
        if gpu_sampler:report['gpu_sampler_stopped']=gpu_sampler.stop()
        if request and content and request.is_file() and request.read_bytes()==content:request.unlink()
        if world and location:
            try:collect_game_logs(location,directory,started_at)
            except OSError as error:
                report['collection']='incomplete';report['errors'].append('Game log collection: '+str(error))
            try:
                exit_review=review_exit_logs(directory,report.get('game_pid'));save(directory/'exit-analysis.json',exit_review)
                report['exit_status']=exit_review['status'];report['exit_exception_count']=len(exit_review['exceptions'])
                report['backend_objects_remaining']=exit_review['backend_objects_remaining']
                notify('Exit status: '+report['exit_status'].upper()+'. Capture completeness and process-exit acceptance are separate; see exit-analysis.json.')
            except (OSError,ValueError,KeyError) as error:
                report['exit_status']='unverified';report['exit_review_error']=str(error)
        clean=not world or report.get('exit_status')=='clean_reported'
        known_cleanup_warning=world and report.get('exit_status')=='cleanup_warning' and report.get('backend_objects_remaining')==40 and report.get('exit_exception_count')==0
        report['overall_acceptance']='pass' if report.get('collection')=='complete' and clean else 'pass_with_warning' if report.get('collection')=='complete' and known_cleanup_warning else 'failed'
        if report['overall_acceptance']=='pass_with_warning':
            report['acceptance_reason']='The capture and Bridge shutdown passed; Remix retained its known 40-object backend warning, reported separately.'
        elif world and report.get('collection')=='complete' and report.get('exit_status')!='clean_reported':
            report['acceptance_reason']='Evidence collection completed, but the game/Host lifecycle did not produce a verified clean exit.'
        elif report['overall_acceptance']=='pass':report['acceptance_reason']='Evidence collection and process-exit lifecycle both passed.'
        else:report['acceptance_reason']='Evidence collection is incomplete.'
        report['collection_phase']='finalized'
        output=archive_result(directory,report)
        print('Evidence collection: '+report['collection'].upper()+('. Analysis artifacts were written.' if report['collection']=='complete' else
              '. Partial evidence only.'),flush=True)
        print('Test acceptance: '+report['overall_acceptance'].upper()+'. '+report['acceptance_reason'],flush=True)
        print('Upload this ONE ZIP:',output,flush=True)
    return output


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--game',type=Path)
    mode=p.add_mutually_exclusive_group();mode.add_argument('--restore',action='store_true');mode.add_argument('--verify-only',action='store_true');mode.add_argument('--beta',action='store_true');mode.add_argument('--performance',action='store_true')
    a=p.parse_args();output=run(game=a.game,restore_only=a.restore,verify_only=a.verify_only,beta=a.beta,performance=a.performance)
    if output:
        summary=json.loads((output.with_suffix('')/'summary.json').read_text(encoding='utf-8'))
        raise SystemExit(0 if summary.get('overall_acceptance') in ('pass','pass_with_warning') else 3 if summary.get('exit_status')=='abnormal' else 2)
