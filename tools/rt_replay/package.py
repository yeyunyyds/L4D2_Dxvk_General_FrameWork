"""Build a verified, self-contained replay launcher using the existing runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'))
sys.path.insert(0,str(ROOT/'tools/rt_replay'))
from replay import verify_package
API_REF='b73eb23053aa50d3787b0c6c8b4b8ae49962880e'
API_SHA='58867d1dbc7a0980e3a5241750f573c9c8b38cb8f9aee1aba4fcb93bd78ec20e'
PRIOR_REF='521f8725c4e7c9b00e5cce1e23a251d270cb108c'
PRIOR_SHA='f0819f95e8c809bf4c35167b5b252bd2452d3ebe057675c0d446219d699f9ef8'
BASE=ROOT/'.research/rt-replay'


def sha(data):return hashlib.sha256(data).hexdigest()


def checked_download(url,expected,limit):
    with urllib.request.urlopen(url,timeout=60) as response:data=response.read(limit+1)
    if len(data)>limit or sha(data)!=expected:raise ValueError('Pinned download hash/size differs: '+url)
    return data


def checked_git_file(ref,name,expected,limit):
    # Actions checkout already supplies scoped HTTPS Git authentication. Raw
    # URLs may return 404 for a private repository; reuse Git, never ask for PATs.
    if len(ref)!=40 or any(c not in '0123456789abcdef' for c in ref):raise ValueError('Invalid immutable Git ref')
    if subprocess.run(['git','cat-file','-e',ref],cwd=ROOT,stderr=subprocess.DEVNULL).returncode:
        subprocess.run(['git','fetch','--no-tags','--depth=1','origin',ref],cwd=ROOT,check=True)
    data=subprocess.check_output(['git','show',ref+':downloads/'+name],cwd=ROOT)
    if len(data)>limit or sha(data)!=expected:raise ValueError('Pinned Git artifact differs: '+name)
    return data


def prepare_header():
    path=BASE/'include/remix/remix_c.h';path.parent.mkdir(parents=True,exist_ok=True)
    data=checked_download(f'https://raw.githubusercontent.com/xoxor4d/dxvk-remix/{API_REF}/public/include/remix/remix_c.h',API_SHA,128*1024)
    path.write_bytes(data)
    commit=os.environ['GITHUB_SHA']
    if len(commit)!=40 or any(c not in '0123456789abcdef' for c in commit):raise ValueError('Full source commit required')
    (BASE/'build-id.h').write_text('#define RT_REPLAY_BUILD "'+commit+'"\n',encoding='ascii')


def package():
    if os.name!='nt':raise OSError('Windows native package checks required')
    commit=os.environ['GITHUB_SHA'];identity='ldb-rt-replay+'+commit
    native=json.loads((BASE/'native-validation.json').read_text(encoding='utf-8-sig'))
    if native['commit']!=commit or any(native.get(k)!='pass' for k in ('geometry_checks','api_material_input_checks','system_nullref_upload','system_d3d9_negative')):raise ValueError('Current native evidence required')
    exe=(BASE/'rt-replay.exe').read_bytes();pe=struct.unpack_from('<I',exe,60)[0]
    if (exe[:2]!=b'MZ' or exe[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',exe,pe+4)[0]!=0x8664
            or identity.encode() not in exe or sha(exe)!=native['exe_sha256']):raise ValueError('Wrong native executable/identity')
    prior=BASE/'prior-product.zip'
    if not prior.is_file() or sha(prior.read_bytes())!=PRIOR_SHA:
        prior.write_bytes(checked_git_file(PRIOR_REF,'rt-world-pilot-dev.zip',PRIOR_SHA,20*1024*1024))
    entries={}
    with zipfile.ZipFile(prior) as archive:
        if archive.testzip():raise ValueError('Prior product CRC failed')
        for name in archive.namelist():
            if name.startswith(('python/','licenses/','payloads/')):entries[name]=archive.read(name)
    for name in ['rt_preflight.py','rt_game_probe.py','rt_suite.py','rt_visual.py','analyze_rt_world.py']:
        entries['scripts/'+name]=(ROOT/'scripts'/name).read_bytes()
    entries['integration/rt/world-runtime.json']=(ROOT/'integration/rt/world-runtime.json').read_bytes()
    entries['tools/rt_replay/replay.py']=(ROOT/'tools/rt_replay/replay.py').read_bytes()
    entries['run-replay.cmd']=(ROOT/'tools/rt_replay/run-replay.cmd').read_bytes()
    entries['README.txt']=(ROOT/'tools/rt_replay/README.txt').read_bytes()
    entries['bin/rt-replay.exe']=exe
    inputs=['integration/rt/rt_runtime_smoke.cpp','.github/workflows/rt-replay.yml','tests/test_gpu_replay.py']
    inputs += [str(p.relative_to(ROOT)).replace('\\','/') for p in sorted((ROOT/'tools/rt_replay').glob('*')) if p.is_file()]
    receipt={'commit':commit,'api_commit':API_REF,'api_sha256':API_SHA,'prior_product_ref':PRIOR_REF,'prior_product_sha256':PRIOR_SHA,
             'inputs_sha256':{name:sha((ROOT/name).read_bytes()) for name in inputs}}
    entries['research/replay-source.json']=(json.dumps(receipt,indent=2)+'\n').encode()
    manifest={'schema':1,'scope':'frozen_geometry_gpu_replay','build_commit':commit,'identity':identity,
              'native_checks':native,'actual_b580_execution':'unrun','game_material_integration':'unrun',
              'routes':['raster','ephemeral','retire-present','persistent','api','api-safe-uv'],
              'materials':['neutral','rough','metal','cutout','alpha','additive','emissive','glass'],
              'files':{name:{'size':len(data),'sha256':sha(data)} for name,data in entries.items()}}
    entries['replay-manifest.json']=(json.dumps(manifest,indent=2)+'\n').encode()
    with tempfile.TemporaryDirectory(prefix='rt-replay-package-') as temporary:
        root=Path(temporary)
        for name,data in entries.items():
            p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        verify_package(root)
        for arguments in [['--help'],['--verify-only']]:
            checked=subprocess.run([str(root/'python/python.exe'),'-I',str(root/'tools/rt_replay/replay.py'),*arguments],capture_output=True,text=True,timeout=30)
            if checked.returncode:raise ValueError('Packaged Python entry failed: '+checked.stderr)
        from rt_preflight import ensure_runtime
        catalog=json.loads(entries['integration/rt/world-runtime.json'])
        cache=ROOT/'.research/world-payload-cache';cache.mkdir(parents=True,exist_ok=True)
        from rt_preflight import check_file
        for part in catalog['payloads']:
            path=cache/part['name']
            if not check_file(path,part):
                path.write_bytes(checked_git_file(part['ref'],part['name'],part['sha256'],95*1024*1024))
        runtime=ensure_runtime(root,catalog,payload_cache=cache)
        positive=BASE/'positive'
        checked=subprocess.run([str(root/'bin/rt-replay.exe'),'--probe',str(runtime/'d3d9.dll'),str(positive.resolve())],capture_output=True,text=True,timeout=60)
        (BASE/'positive-probe.log').write_text(checked.stdout+'\n'+checked.stderr,encoding='utf-8')
        probe=json.loads((positive/'status.json').read_text())
        if checked.returncode or probe['api_probe']!='pass' or probe['identity']!=identity:raise ValueError('Pinned runtime/API positive probe failed')
    output=ROOT/'.research/dist/rt-replay';output.mkdir(parents=True,exist_ok=True)
    product=output/'rt-gpu-replay-dev.zip'
    with zipfile.ZipFile(product,'w',zipfile.ZIP_DEFLATED) as archive:
        for name,data in entries.items():archive.writestr(name,data)
    with zipfile.ZipFile(product) as archive:
        if archive.testzip() or set(archive.namelist())!=set(entries):raise ValueError('Product CRC/member set mismatch')
        for name,data in entries.items():
            if archive.read(name)!=data:raise ValueError('Product member changed')
    validation={'schema':1,'commit':commit,'package_validation':'pass','launcher':product.name,'payloads':[],
                'primary_sha256':sha(product.read_bytes()),'primary_size':product.stat().st_size,
                'packaged_python_isolated_entry':'pass','pinned_runtime_api_positive':'pass','system_d3d9_negative':'pass',
                'system_nullref_upload':'pass','actual_b580_execution':'unrun','game_material_integration':'unrun'}
    (output/'publish-index.json').write_text(json.dumps(validation,indent=2)+'\n')
    print(json.dumps(validation,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--prepare-header',action='store_true');args=parser.parse_args()
    if args.prepare_header:prepare_header()
    else:package()
