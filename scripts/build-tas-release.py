#!/usr/bin/env python3
"""Pull-and-build release driver, invoked only after local changes are pushed.
Usage: build-tas-release.py EXPECTED_COMMIT OUTPUT_DIRECTORY
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

checkout = Path('/home/openwrtbuild/immortalwrt-dtu')
commit, output_arg = sys.argv[1:3]
output = Path(output_arg).resolve()
assert output.is_relative_to(Path('/home/openwrtbuild/releases'))
output.mkdir(mode=0o700, parents=True, exist_ok=False)
lock = open('/home/openwrtbuild/.dtu-release.lock', 'a')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)


def status(stage, **extra):
    temporary = output / 'status.tmp'
    temporary.write_text(json.dumps(dict(stage=stage, commit=commit, pid=os.getpid(), **extra)) + '\n')
    temporary.replace(output / 'status.json')


def git(*args):
    return subprocess.check_output(['git', *args], cwd=checkout, text=True).strip()


def run(args, label, timeout=14400):
    status(label)
    with (output / (label + '.log')).open('wb') as log:
        subprocess.run(args, cwd=checkout, stdout=log, stderr=subprocess.STDOUT,
                       timeout=timeout, check=True)


try:
    assert git('status', '--porcelain', '--untracked-files=all') == '', 'checkout is not clean'
    assert git('branch', '--show-current') == 'codex/immortalwrt-25.12.2'
    run(['git', 'pull', '--ff-only', 'origin', 'codex/immortalwrt-25.12.2'], 'pull', 300)
    assert git('rev-parse', 'HEAD') == commit
    assert git('status', '--porcelain', '--untracked-files=all') == ''
    old_config = (checkout / '.config').read_text() if (checkout / '.config').exists() else ''
    run(['./scripts/feeds', 'update', '-a'], 'feeds-update', 1800)
    run(['./scripts/install-tas-feeds.sh'], 'feeds-install', 1800)
    for feed, revision in (('packages', '84bd86384928955b568988ca0e09e2c78c75173d'),
                           ('luci', 'd6167ea0645cbd1327708d85f94824f42d0eb872')):
        assert git('-C', 'feeds/' + feed, 'rev-parse', 'HEAD') == revision
    shutil.copy2(checkout / 'configs/tastek-mt76x8.config', checkout / '.config')
    run(['make', 'defconfig'], 'defconfig', 1200)
    if 'CONFIG_TARGET_ramips_mt76x8=y' not in old_config:
        run(['make', 'clean'], 'target-change-clean', 1800)
    run(['make', '-j16'], 'image-build')
    target = checkout / 'bin/targets/ramips/mt76x8'
    names = ['immortalwrt-ramips-mt76x8-' + profile + '-squashfs-sysupgrade.bin'
             for profile in ('tastek_it692', 'tastek_it694')]
    for name in names + ['immortalwrt-ramips-mt76x8.manifest', 'sha256sums', 'profiles.json',
                          'config.buildinfo', 'feeds.buildinfo', 'version.buildinfo']:
        shutil.copy2(target / name, output / name)
    shutil.copy2(checkout / 'configs/tastek-mt76x8.config', output / 'seed.config')
    shutil.copy2(checkout / '.config', output / 'build.config')
    (output / 'source.commit').write_text(commit + '\n')
    run(['python3', 'scripts/verify-tas-release.py', str(checkout), str(output)], 'verify-images', 1200)
    assert git('status', '--porcelain', '--untracked-files=all') == ''
    status('images-verified', images=names)
except BaseException as error:
    status('failed', reason=str(error))
    raise
