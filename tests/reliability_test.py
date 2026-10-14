#!/usr/bin/env python3
"""Local disposable fixtures: publication crashes, corruption and source preservation.

Usage: python3 tests/reliability_test.py ./exfat2ampr-host
No console, network or existing game is used. Fixtures and logs are kept in build/.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
(root / 'build').mkdir(exist_ok=True)
work = Path(tempfile.mkdtemp(prefix='reliability-', dir=root / 'build')).resolve()
assert work.is_relative_to(root / 'build')
source = work / 'fixture'
for folder in ('sce_sys', 'data', 'audio'):
    (source / folder).mkdir(parents=True, exist_ok=True)
(source / 'sce_sys/param.json').write_text(json.dumps({'titleId': 'PPSA00001', 'titleName': 'Reliability fixture', 'contentVersion': '01.000.000'}))
(source / 'eboot.bin').write_bytes(b'fixture libSceAmpr.sprx libSceKernel.sprx\0' * 128)
(source / 'data/a.dat2').write_bytes(bytes(range(256)) * 16384)
(source / 'data/b.dat2').write_bytes(b'asset contents\0' * 200000)
(source / 'audio/bank.wem').write_bytes(bytes(range(256)) * 1024)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

original = {p.relative_to(source).as_posix(): digest(p) for p in source.rglob('*') if p.is_file()}
sequence = 0
def run(*args, want=0, crash=None):
    global sequence
    sequence += 1
    env = os.environ.copy()
    env.pop('E2A_TEST_CRASH', None)
    if crash: env['E2A_TEST_CRASH'] = crash
    log = work / f'{sequence:02d}.log'
    with log.open('wb') as output:
        result = subprocess.run([str(binary), *args], cwd=work, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=180)
    assert result.returncode == want, (args, result.returncode, want, log.read_bytes()[-2500:])
    return result

for point in ('publish_volume', 'publish_manifest', 'copy_committed', 'verify_start'):
    run('fixture', 'homebrew', point, crash=point, want=86)
    run('--resume', f'homebrew/{point}')
    run('--verify', f'homebrew/{point}')
    print('publication recovery:', point, 'OK', flush=True)

run('--pause-after-mb', '1', 'fixture', 'homebrew', 'paused', want=3)
run('--resume', 'homebrew/paused')
run('--verify', 'homebrew/paused')
print('mid-file pause/resume: OK', flush=True)

run('--remove-source', 'homebrew/paused', 'homebrew', 'reconverted', want=1)
assert (work / 'homebrew/paused/ampr_assets.index').is_file()
assert not (work / 'homebrew/reconverted').exists()
print('converted source refused and preserved: OK', flush=True)

loose = work / 'homebrew/paused/audio/bank.wem'
data = bytearray(loose.read_bytes()); data[100] ^= 1; loose.write_bytes(data)
run('--verify', 'homebrew/paused', want=1)
print('same-size loose corruption detected: OK', flush=True)

manifest = work / 'homebrew/paused/ampr_assets.index'
index = work / 'homebrew/paused/ampr_emu.index'
before = digest(index)
data = bytearray(manifest.read_bytes()); data[-2] ^= 1; manifest.write_bytes(data)
run('--repair', 'homebrew/paused', want=1)
assert digest(index) == before
print('corrupt manifest repair refused without index changes: OK', flush=True)

run('--pause-after-mb', '1', 'fixture', 'homebrew', 'bad_checkpoint', want=3)
checkpoint = work / 'homebrew/bad_checkpoint/.e2a-state/state.bin'
data = bytearray(checkpoint.read_bytes()); data[48] ^= 0x7f; checkpoint.write_bytes(data)
run('--resume', 'homebrew/bad_checkpoint', want=1)
assert checkpoint.is_file()
print('damaged checkpoint refused and preserved: OK', flush=True)

assert all(digest(source / p) == value for p, value in original.items())
print('source unchanged; logs:', work, flush=True)
