#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare a fresh project manifest and retained public input profile; no native execution.

configure.py subsequently creates the world through ordinary guarded native
authoring. This step does not supply a prebuilt game, physics or cooked content.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import uuid

ROOT=Path(__file__).resolve().parents[2]
PROFILE=ROOT/'docs/evidence/fixtures/agent-switchback-fresh/settings/range.poima-input.json'


def identity(path):
    with Path(path).open('rb')as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
    return dict(bytes=Path(path).stat().st_size,sha256=digest)


def prepare(output):
    output=Path(output).absolute()
    if output.exists()or output.is_symlink()or not output.parent.is_dir():
        raise ValueError('Fresh output with an existing parent required.')
    if PROFILE.is_symlink()or not PROFILE.is_file():raise ValueError('Regular public input profile required.')
    before=identity(PROFILE);output.mkdir();(output/'settings').mkdir()
    destination=output/'settings/range.poima-input.json';shutil.copyfile(PROFILE,destination)
    if identity(destination)!=before or identity(PROFILE)!=before:raise RuntimeError('Original input profile changed.')
    project=dict(format='poima.project',version=1,project_id=uuid.uuid4().hex,name='Projectile Range',
        entry=dict(world='world.json',camera=f'{3:032x}',controller=f'{2:032x}'),
        input_profile='settings/range.poima-input.json',audio=False)
    (output/'project.json').write_text(json.dumps(project,indent=2)+'\n',encoding='utf-8')
    record=dict(format='poima.projectile-range-source.v1',input_profile=before,
        public_source='docs/evidence/fixtures/agent-switchback-fresh/settings/range.poima-input.json',
        project=identity(output/'project.json'),world_present=False,
        scope='New manifest and byte-identical public input profile only. Run guarded configure.py before project inspection/adoption; no native/build/render execution.')
    (output/'projectile-source.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    return record


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();print(json.dumps(prepare(args.output)))
