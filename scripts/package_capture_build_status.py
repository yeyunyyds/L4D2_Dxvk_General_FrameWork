"""Publish CI outcome/logs as ZIP so the download branch contains only ZIP files."""
import hashlib
import json
import os
from pathlib import Path
import zipfile
from prepare_research import ROOT

out = ROOT / '.research/dist'
out.mkdir(parents=True, exist_ok=True)
package = out / 'ldb-host-capture-dev.zip'
status = {
    'framework_commit': os.environ.get('GITHUB_SHA'),
    'run_id': os.environ.get('GITHUB_RUN_ID'),
    'run_attempt': os.environ.get('GITHUB_RUN_ATTEMPT'),
    'native_build_outcome': os.environ.get('BUILD_OUTCOME', 'unknown'),
    'package_sha256': hashlib.sha256(package.read_bytes()).hexdigest() if package.exists() else None,
    'l4d2_game_capture': 'unrun',
    'b580_integrated_capture': 'unrun',
    'gpu_fg_execution': 'unrun',
}
with zipfile.ZipFile(out / 'ldb-host-capture-build-status.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('status.json', json.dumps(status, indent=2) + '\n')
    log = ROOT / 'native-build.log'
    if log.is_file(): archive.write(log, 'native-build.log')
    for path in sorted((ROOT / '.research/ldb-capture-source').glob('bridge/_captureRelease_*/meson-logs/*.txt')):
        archive.write(path, path.relative_to(ROOT / '.research/ldb-capture-source').as_posix())
print(json.dumps(status, indent=2))
