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
        if (runtime.get('scope')!='rt_prerequisites' or runtime['runtime_files']['d3d9.dll']['sha256']!=
                '843e63f99d63f64155dc359cb5f2b9aa10058e2f6510358054f83920d8589ca4'):
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


def finish_world_capture(directory,game,since,report,notify,beta=False,performance=False):
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
        report['performance']='arc_stability_collected' if performance_review['pass'] else 'incomplete'
        report['preview']='completed_performance_stability' if performance_review['pass'] else 'incomplete'
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
        report['result_scope']='128-frame Arc direct-light stability validation; 8-frame staged warm-up then 120 frames fixed at 32 GPU-skinned meshes; CPU-pose explicit RT meshes omitted; not image acceptance'
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
    ends=[e for e in events if e.get('event')=='preview_end']
    if len(ends)!=1 or not ends[0].get('complete') or ends[0].get('frames',0)<(128 if stability else 120) or any(ends[0].get(k)!=0 for k in ('entries','pending_fences','held')):errors.append('performance_completion')
    elif ends[0].get('submitted')!=ends[0].get('skinned_submitted'):errors.append('performance_cpu_pose_submission')
    for e in events:
        if e.get('code',0):errors.append('performance_runtime_failure:'+e.get('event','unknown'))
    mesh=[e for e in events if e.get('event')=='api_mesh_frame' and e.get('gpu_skin_performance_phase',-1)>=0]
    expected=[32]*4 if stability else [16,32,48,64]
    if [e.get('gpu_skin_performance_phase') for e in mesh]!=list(range(4)) or [e.get('skinned_frame_limit') for e in mesh]!=expected:
        errors.append('performance_phase_sequence')
    elif any(not isinstance(e.get('skinned_frame_submitted'),int) or not 0<=e['skinned_frame_submitted']<=e['skinned_frame_limit'] or
             not isinstance(e.get('skinned_frame_unique_meshes'),int) or not isinstance(e.get('skinned_frame_repeat_draws'),int) or
             e['skinned_frame_unique_meshes']!=e['skinned_frame_submitted'] or e['skinned_frame_repeat_draws']!=0 or
             e.get('skinned_frame_max')!=(32 if stability else e.get('skinned_frame_max')) or
             not isinstance(e.get('preview_submitted'),int) or e['preview_submitted']!=e.get('preview_skinned_submitted') for e in mesh):
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
    for index,limit in enumerate(expected):
        name='gpu_stability_'+str(index+1) if stability else 'gpu_ramp_'+str(limit)
        timing_name=name if stability else ('gpu_ramp_64' if limit==48 else 'gpu_ramp_128' if limit==64 else name) if legacy_timing else name
        row=windows.get(timing_name);wall=row.get('wall_ms',0) if row else 0
        if not isinstance(wall,(int,float)) or not math.isfinite(wall) or wall<=0:errors.append('performance_timing:'+name);continue
        evidence=mesh[index] if len(mesh)>index else {};current={k:evidence.get(k) for k in ('preview_skinned_submitted','preview_skinned_created','preview_skinned_reused')}
        if not all(isinstance(value,int) and value>=previous.get(key,0) for key,value in current.items()):errors.append('performance_gpu_skin_accounting:'+name)
        submitted_delta=current.get('preview_skinned_submitted',0)-previous.get('preview_skinned_submitted',0)
        created_delta=current.get('preview_skinned_created',0)-previous.get('preview_skinned_created',0)
        reused_delta=current.get('preview_skinned_reused',0)-previous.get('preview_skinned_reused',0)
        if submitted_delta<=0 or created_delta+reused_delta!=submitted_delta:errors.append('performance_gpu_skin_not_exercised:'+name)
        phase={'gpu_skin_limit':limit,'frames':30,'wall_ms':wall,'fps':30000/wall,
                       'skinned_frame_submitted':evidence.get('skinned_frame_submitted',0),
                       'skinned_frame_unique_meshes':evidence.get('skinned_frame_unique_meshes',0),
                       'skinned_frame_repeat_draws':evidence.get('skinned_frame_repeat_draws',0),
                       'gpu_skin_submitted_delta':submitted_delta,
                       'gpu_skin_created_delta':created_delta,
                       'gpu_skin_reused_delta':reused_delta,
                       'mesh_created_delta':evidence.get('created',0)-previous.get('created',0),
                       'mesh_reused_delta':evidence.get('reused',0)-previous.get('reused',0),
                       'model_ms':row.get('scopes',{}).get('model',{}).get('elapsed_ms',0),
                       'cpu_skin_ms':row.get('scopes',{}).get('skin',{}).get('elapsed_ms',0),
                       'mesh_create_ms':row.get('scopes',{}).get('mesh_create',{}).get('elapsed_ms',0),
                       'present_ms':row.get('scopes',{}).get('present',{}).get('elapsed_ms',0)}
        if isinstance(row.get('utc_filetime_100ns'),int):
            end_ns=(row['utc_filetime_100ns']-116444736000000000)*100;start_ns=end_ns-int(wall*1000000);values=[]
            for sample in gpu_rows:
                if start_ns<=sample.get('unix_ns',0)<=end_ns:
                    engines=[e.get('percent',0) for e in sample.get('engines',[]) if e.get('instance','').endswith('engtype_3D') and isinstance(e.get('percent'),(int,float)) and math.isfinite(e['percent'])]
                    if engines:values.append(max(engines))
            if values:
                ordered=sorted(values);phase['gpu_3d_engine']={'samples':len(values),'average_percent':sum(values)/len(values),'p50_percent':ordered[len(ordered)//2],'p95_percent':ordered[int((len(ordered)-1)*.95)],'max_percent':max(values)}
        phases.append(phase);previous=dict(evidence)
    best=max((p['fps'] for p in phases),default=0);worst=min((p['fps'] for p in phases),default=0)
    return {'schema':1,'pass':not errors,'errors':sorted(set(errors)),'profile':'arc_direct_stability' if stability else 'legacy_gpu_skin_ramp','phases':phases,'best_fps':best,'worst_fps':worst,'meets_24_fps':bool(phases) and worst>=24,
            'end':ends[-1] if ends else None,'limits':'v35 uses an 8-frame staged warm-up followed by four fixed-32 30-frame windows under a fixed direct-light TAAU profile. Performance mode omits unsupported/excess CPU-pose models, so image completeness is not evaluated. GPU PDH counters remain separate per engine.' if stability else 'Sequential phases share a changing scene and are directional, not matched A/B. Performance mode omits unsupported/excess CPU-pose models from explicit RT, so image completeness is not evaluated. GPU PDH counters remain separate per engine.'}

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
            if performance:config_options += ['rtx.upscalerType = 3','rtx.resolutionScale = 0.75','rtx.enableDirectLighting = True',
                'rtx.enableSecondaryBounces = False','rtx.useRTXDI = False','rtx.integrateIndirectMode = 0']
            config='\n'.join(config_options)+'\n'
            (directory/'world.conf').write_text(config,encoding='utf-8')
            report['scope']='arc_direct_light_performance_stability' if performance else 'five_minute_complete_coverage_gameplay_beta' if beta else 'selected_static_world_runtime_pilot'
            report['game_rt']='attempted_requires_review'
        if request.exists():raise ValueError('Another observation request exists. Preserve it and finish its collector first: '+str(request))
        with request.open('xb') as f:f.write(content)
        notify('Enter a map, return to the game, and press F9 once. The '+('30-frame performance gate' if performance else '30-frame beta gate' if beta else '600-Present capture' if world else '120-Present capture')+' starts automatically.')
        if performance:notify('RT Performance v35: one automatic 128-frame Arc stability run. It warms from 4 to 32 GPU-skinned meshes over 8 frames, then holds 32 for four measured 30-frame windows. The fixed profile uses TAAU at 0.75 resolution, direct light only, secondary bounces off and RTXDI off. Unsupported and excess CPU-pose models are intentionally omitted. Keep the same scene and do not press F9 again.')
        elif beta:notify('RT Beta v30: after the 30-frame safety gate, play normally for five minutes. GPU skinning keeps 16 submissions per frame at ordinary model density and a 4-submission baseline above 256 model draws; excess models use CPU poses. Do not press F9 again. Exit only when this window asks.')
        elif world:notify('World Pilot v30: keep animated characters nearby, then press F9 ONCE. Move and turn slowly. GPU skinning keeps 16 submissions per frame at ordinary model density and 4 above 256 model draws; excess models use CPU poses. Exit only when this window asks; keep it open until the final ONE ZIP path appears.')
        if not game_pids():os.startfile('steam://rungameid/550')
        done=directory/f'rt-draw-done-{session}.json'
        deadline=time.monotonic()+900;last=0;pid=None;last_alive=time.monotonic();startup_saved=False;first_alive=None
        while time.monotonic()<deadline and not done.exists():
            process_diagnostics={};candidates=game_pids(process_diagnostics)
            if gpu_sampler:gpu_sampler.track([p['pid'] for p in process_diagnostics.get('related_processes',[]) if p['exe'].casefold()=='l4d2bridge64.exe'])
            if len(candidates)>1:raise ValueError('Multiple L4D2 processes found')
            if candidates:
                pid=candidates[0];last_alive=time.monotonic()
                report['game_pid']=pid
                if first_alive is None:first_alive=last_alive
                if world and manifest.get('pilot_revision',0)>=24 and gpu_sampler is None:
                    gpu_sampler=Sampler(directory).start();gpu_sampler.track([pid])
                if world and not startup_saved and last_alive-first_alive>=5:
                    startup_saved=True
                    try:
                        startup_modules(directory,pid,notify)
                        if gpu_sampler:
                            modules=json.loads((directory/'startup-modules.json').read_text(encoding='utf-8'))
                            gpu_sampler.track([x['pid'] for x in modules['processes']])
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
            notify('30-frame gate passed. The 8-frame warm-up and fixed-32 Arc stability run are active; keep the scene and camera movement representative.' if performance else '30-frame safety gate passed. Five-minute RT Beta is active; play normally and watch for missing geometry, deformation, freezes or crashes.' if beta else '600-Present diagnostic saved. Live preview first30s has no AA, last30s requests native TAA. Observe edges/trails; no second keypress.')
            preview_done=directory/f'rt-preview-done-{session}.json';preview_deadline=time.monotonic()+(240 if performance else 330 if beta else 90)
            while not preview_done.is_file() and time.monotonic()<preview_deadline:
                if pid not in game_pids():break
                time.sleep(.5)
            preview_complete=completed_preview(preview_done,session)
            report['preview']='collected_requires_review' if preview_complete else 'incomplete'
            if preview_complete:notify(('Arc stability run finished.' if performance else 'Five-minute Beta segment finished.' if beta else 'Preview segment finished.')+' Collecting final evidence; exit only when requested below.')
            elif preview_done.is_file():notify('Preview stopped early and retained partial evidence. Collecting final diagnostics; this run has not passed.')
        if gpu_sampler:
            report['gpu_sampler_stopped']=gpu_sampler.stop();gpu_sampler=None
        finish_capture(directory,root,pid,report,notify)
        if not marker_ok:
            report['collection']='incomplete';report['errors'].append('Capture completion marker identity/count/reason mismatch')
        if world:
            finish_world_capture(directory,location,started_at,report,notify,beta=beta,performance=performance)
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
        report['collection_phase']='finalized'
        output=archive_result(directory,report)
        print('Collection: '+report['collection'].upper()+('. RT images still require review.' if report['collection']=='complete' else
              '. Partial evidence only; the test has NOT passed.'),flush=True)
        print('Upload this ONE ZIP:',output,flush=True)
    return output


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--game',type=Path)
    mode=p.add_mutually_exclusive_group();mode.add_argument('--restore',action='store_true');mode.add_argument('--verify-only',action='store_true');mode.add_argument('--beta',action='store_true');mode.add_argument('--performance',action='store_true')
    a=p.parse_args();output=run(game=a.game,restore_only=a.restore,verify_only=a.verify_only,beta=a.beta,performance=a.performance)
    if output:
        summary=json.loads((output.with_suffix('')/'summary.json').read_text(encoding='utf-8'))
        raise SystemExit(3 if summary.get('exit_status')=='abnormal' else 0 if summary['collection']=='complete' else 2)
