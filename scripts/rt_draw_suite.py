"""Install the paired observer, collect one F9 burst, and retain one result ZIP."""
import argparse
import hashlib
import json
import os
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

TARGETS=('bin/dxvk_d3d9.dll','bin/.l4d2bridge/L4D2Bridge64.exe')


def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):Path(path).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


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
        if not receipt.is_file():raise ValueError('Installed observer matches, but its restore receipt is missing')
        old=json.loads(receipt.read_text(encoding='utf-8'))
        if Path(old['game'])!=game:raise ValueError('Installation receipt belongs to another game directory')
        return old
    for name in TARGETS:
        if not (game/name).is_file():raise ValueError('LDB 1.3.2 installation not found: '+name)
        if digest(root/'payload'/name)!=manifest['binaries'][name]['sha256']:raise ValueError('Observer binary checksum mismatch: '+name)
    backup=root/'backups'/secrets.token_hex(8);backup.mkdir(parents=True)
    old={'game':str(game),'files':{},'scope':'paired_draw_observer_only'}
    for name in TARGETS:
        source=game/name;destination=backup/name;destination.parent.mkdir(parents=True)
        shutil.copy2(source,destination)
        old['files'][name]={'backup':str(destination),'previous_sha256':digest(destination),
                            'installed_sha256':manifest['binaries'][name]['sha256']}
    # The recovery receipt exists before the first game file is changed.
    save(receipt,old)
    changed=[]
    try:
        for name in TARGETS:
            replace_copy(root/'payload'/name,game/name);changed.append(name)
        for name in TARGETS:
            if digest(game/name)!=old['files'][name]['installed_sha256']:raise ValueError('Installed pair verification failed')
    except Exception:
        for name in reversed(changed):
            if digest(game/name)==old['files'][name]['installed_sha256']:
                replace_copy(Path(old['files'][name]['backup']),game/name)
        raise
    return old


def restore(root):
    receipt=json.loads((root/'installed-observer.json').read_text(encoding='utf-8'));game=Path(receipt['game'])
    for name,entry in receipt['files'].items():
        if digest(entry['backup'])!=entry['previous_sha256']:raise ValueError('Backup checksum mismatch')
        if digest(game/name) not in (entry['installed_sha256'],entry['previous_sha256']):raise ValueError('Game files changed after installation; preserve them: '+name)
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
        if not printed:notify('Please exit L4D2 normally. Waiting before replacing the client/Host pair...');printed=True
        if time.monotonic()>deadline:raise TimeoutError('Game or Host is still running; no installation performed')
        time.sleep(1)


def archive_result(directory,report):
    files=[p for p in directory.rglob('*') if p.is_file()]
    if sum(p.stat().st_size for p in files)>192*1024*1024:
        report['collection']='incomplete';report['errors'].append('result_budget_exceeded')
    save(directory/'summary.json',report)
    output=directory.with_suffix('.zip')
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED) as z:
        used=0;omitted=[]
        for path in sorted(directory.rglob('*')):
            if not path.is_file():continue
            if used+path.stat().st_size>192*1024*1024 and path.name!='summary.json':
                omitted.append(str(path.relative_to(directory)));continue
            z.write(path,path.relative_to(directory));used+=path.stat().st_size
        z.writestr('omitted-files.json',json.dumps(omitted))
    return output


def run(root=ROOT,game=None,restore_only=False):
    if os.name!='nt':raise OSError('This launcher requires Windows x64')
    if restore_only:
        wait_stopped(print);location=restore(root);print('Original client/Host restored:',location);return
    manifest=json.loads((root/'draw-observer-manifest.json').read_text(encoding='utf-8'))
    for name,entry in manifest['files'].items():
        if digest(root/name)!=entry['sha256']:raise ValueError('Package checksum mismatch: '+name)
    name='rt-draw-'+time.strftime('%Y%m%d-%H%M%S')+'-'+secrets.token_hex(4)
    directory=root/'results'/name;directory.mkdir(parents=True)
    report={'schema':1,'scope':'actual_draw_consumer_observation','build_commit':manifest['build_commit'],
            'collection':'incomplete','errors':[],'game_rt':'unrun','source_queue_execution':'unrun',
            'source_parallel_preparation':'unrun','performance':'unrun'}
    request=None;content=None
    def notify(message):
        print(message,flush=True)
        with (directory/'progress.log').open('a',encoding='utf-8') as f:f.write(time.strftime('%H:%M:%S ')+message+'\n')
    try:
        pids=game_pids();location=locate_game(game,pids,root)
        if not (location/'left4dead2.exe').is_file():raise ValueError('Invalid L4D2 directory')
        matches=all((location/n).is_file() and digest(location/n)==manifest['binaries'][n]['sha256'] for n in TARGETS)
        if not matches:wait_stopped(notify)
        receipt=install(root,location,manifest);save(directory/'installation.json',receipt)
        session=secrets.randbits(63) or 1;request=location/'bin/ldb-rt-draw.request'
        content=f'LDB_RT_DRAW_1\n{session:x}\n{directory.resolve()}\n'.encode('utf-8')
        if request.exists():raise ValueError('Another observation request exists. Preserve it and finish its collector first: '+str(request))
        with request.open('xb') as f:f.write(content)
        notify('Enter a map, return to the game, and press F9 once. The 120-Present capture stops automatically.')
        if not game_pids():os.startfile('steam://rungameid/550')
        done=directory/f'rt-draw-done-{session}.json'
        deadline=time.monotonic()+900;last=0;pid=None
        while time.monotonic()<deadline and not done.exists():
            candidates=game_pids()
            if len(candidates)>1:raise ValueError('Multiple L4D2 processes found')
            if candidates:pid=candidates[0]
            if time.monotonic()-last>=15:
                notify('Waiting for F9 / capture completion; game PID: '+str(pid));last=time.monotonic()
            time.sleep(.5)
        if not done.is_file():raise TimeoutError('No completed capture; partial logs are retained')
        notify('Capture ended. Reading module and queue ABI evidence...')
        if pid:
            binding=collect(pid,json.loads((root/'research/rt-source-audit.json').read_text()))
            save(directory/'game-binding.json',binding)
        traces={p.name:analyze(p) for p in directory.glob('rt-draw-host-*.bin')}
        save(directory/'draw-analysis.json',traces)
        report['draw_observation']='pass' if traces and all(t['pass'] for t in traces.values()) else 'fail_or_partial'
        report['collection']='complete'
        report['result_scope']='consumer state / RPC ordering only; no Source producer queue or RT image acceptance'
    except Exception as error:
        report['errors'].append(f'{type(error).__name__}: {error}');notify(report['errors'][-1])
    finally:
        if request and content and request.is_file() and request.read_bytes()==content:request.unlink()
        output=archive_result(directory,report);print('Upload this ONE ZIP:',output,flush=True)
    return output


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--game',type=Path);p.add_argument('--restore',action='store_true')
    a=p.parse_args();run(game=a.game,restore_only=a.restore)
