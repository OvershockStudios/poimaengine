#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Copy the licensed public Switchback scene into a fresh FPS Feedback project.

This performs no downloads, compilation, native execution or fixture mutation.
Gameplay is built separately from FpsFeedbackGame.cs; historical game source and
its verifier are deliberately not copied into the new project.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import stat
import uuid

ROOT = Path(__file__).resolve().parents[2]
BASIS = ROOT/'docs/evidence/fixtures/agent-switchback-fresh'
MEMBERS = ('project.json', 'world.json', 'world.json.assets', 'settings', 'content')


def identity(path):
    raw = path.read_bytes()
    return dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def inputs():
    result = {}
    for label in MEMBERS:
        path = BASIS/label
        for member in ([path] if path.is_file() else sorted(path.rglob('*'))):
            info = member.lstat()
            if member.is_symlink() or not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
                raise ValueError('Expected ordinary licensed fixture member: '+str(member))
            if member.is_file(): result[member.relative_to(BASIS).as_posix()] = identity(member)
    return result


def prepare(output):
    output = Path(output).absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ValueError('Output must be fresh with an existing parent.')
    # A new sibling project must never overwrite or contain the preserved source.
    if BASIS.resolve().is_relative_to(output.resolve()) or output.resolve().is_relative_to(BASIS.resolve()):
        raise ValueError('Output must be independent of the historical fixture.')
    original = inputs()
    output.mkdir()
    for label in MEMBERS:
        source = BASIS/label
        if source.is_dir(): shutil.copytree(source, output/label)
        else: shutil.copyfile(source, output/label)
    for label, expected in original.items():
        if identity(output/label) != expected:
            raise RuntimeError('Independent copy differs: '+label)
    project = json.loads((output/'project.json').read_bytes())
    project['project_id'] = uuid.uuid4().hex
    project['name'] = 'FPS Feedback Range'
    (output/'project.json').write_text(json.dumps(project, indent=2)+'\n', encoding='utf-8')
    if inputs() != original:
        raise RuntimeError('Historical input changed during copying.')
    record = dict(format='poima.fps-feedback-source.v1', basis='agent-switchback-fresh',
        copied_inputs=original, adapted_manifest=identity(output/'project.json'),
        adaptation='Only independent project ID/name changed. All world fields, source assets, input receipts and notices preserved.',
        qualification='Fresh copied project only; no compiled gameplay, simulation or rendering performed.')
    (output/'feedback-source.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = prepare(args.output)
    print(json.dumps(dict(project=str(args.output/'project.json'), copied_files=len(result['copied_inputs']))))


if __name__ == '__main__': main()
