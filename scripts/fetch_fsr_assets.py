"""Read immutable, hash-checked Windows FSR assets from the ZIP-only branch."""
import hashlib
import io
import json
import re
import subprocess
import zipfile
from prepare_research import ROOT

def obtain_assets():
    lock=json.loads((ROOT/'research/fsr-backend-assets.json').read_text())
    for name,expected in lock['project_source_hashes'].items():
        if hashlib.sha256((ROOT/name).read_bytes()).hexdigest()!=expected:
            raise RuntimeError(f'FSR binary does not match project source: {name}')
    revision=lock['download_commit']
    if not re.fullmatch('[0-9a-f]{40}',revision):raise ValueError('Invalid asset commit')
    cache=ROOT/'.research/fsr-assets'/f'{revision}.zip'
    if not cache.exists():
        subprocess.run(['git','fetch','--depth=1','origin',revision],cwd=ROOT,check=True)
        data=subprocess.check_output(['git','show',f'{revision}:downloads/fsr-backend-windows-x64.zip'],cwd=ROOT)
    else:data=cache.read_bytes()
    if hashlib.sha256(data).hexdigest()!=lock['archive_sha256']:raise RuntimeError('FSR asset ZIP hash mismatch')
    if len(data)>16*1024*1024:raise RuntimeError('Unexpected asset archive size')
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        if len(z.namelist())!=len(set(z.namelist())) or set(z.namelist())!=set(lock['files']) or z.testzip():
            raise RuntimeError('FSR asset contents mismatch')
        files={name:z.read(name) for name in z.namelist()}
        for name,payload in files.items():
            if hashlib.sha256(payload).hexdigest()!=lock['files'][name]:raise RuntimeError('FSR asset file hash mismatch: '+name)
    cache.parent.mkdir(parents=True,exist_ok=True)
    if not cache.exists():cache.write_bytes(data)
    return lock,files

if __name__=='__main__':
    lock,files=obtain_assets()
    print('Verified immutable Windows FSR assets:',lock['archive_sha256'])
