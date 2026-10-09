#!/usr/bin/env python3
"""Pinned character extension: exact output, idempotent writes, source drift rejection."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,required=True)
parser.add_argument('--cmake',default='cmake')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
patch=root/'cmake/jolt_character_ids.cmake'
expected={'Character.h':'9dceba7e28d98d2589a007a3a7152916f855972d49a12bfbf21c62f6fda21ac0',
          'Character.cpp':'7cc3531d0fe30de331a4cf4edbe9b7a05032b88cd3772432adfecce4e4554b07'}
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
with tempfile.TemporaryDirectory(prefix='poima-jolt-patch-') as temporary:
    base=Path(temporary)/'source with spaces'
    target=base/'Jolt/Physics/Character';target.mkdir(parents=True)
    for name in expected:shutil.copyfile(args.source/'Jolt/Physics/Character'/name,target/name)
    script=Path(temporary)/'check.cmake'
    script.write_text(f'include([=[{patch.as_posix()}]=])\npoima_patch_jolt_character_ids([=[{base.as_posix()}]=])\n')
    def run():return subprocess.run([args.cmake,'-P',str(script)],capture_output=True,text=True,timeout=20)
    first=run();assert first.returncode==0,first.stderr
    for name,digest in expected.items():assert sha(target/name)==digest,name
    times={name:(target/name).stat().st_mtime_ns for name in expected}
    second=run();assert second.returncode==0,second.stderr
    for name,digest in expected.items():
        assert sha(target/name)==digest,name
        assert (target/name).stat().st_mtime_ns==times[name],name+' rewritten on repeated configuration'
    header=target/'Character.h';header.write_bytes(header.read_bytes()+b'\n// unknown downstream edit\n')
    before={name:sha(target/name) for name in expected}
    rejected=run();assert rejected.returncode!=0 and 'refusing automatic patch' in rejected.stderr,rejected.stderr
    assert {name:sha(target/name) for name in expected}==before,'Unsupported source was modified'
print('Pinned character patch output, idempotence and drift rejection passed.')
