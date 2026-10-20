#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare an independent licensed FPS scene; no build/native execution."""
import argparse
import importlib.util
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def basis():
    spec=importlib.util.spec_from_file_location('weapon_actions_scene_basis',ROOT/'examples/fps-feedback/prepare.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def prepare(output):
    result=basis().prepare(output);project=Path(output)/'project.json';value=json.loads(project.read_bytes());value['name']='Weapon Actions Range';project.write_text(json.dumps(value,indent=2)+'\n')
    record=dict(format='poima.weapon-actions-source.v1',basis_preparation=result,copied_inputs=result['copied_inputs'],adapted_manifest=basis().identity(project),qualification='Independent scene copy/name only; no build/native execution.')
    (Path(output)/'weapon-source.json').write_text(json.dumps(record,indent=2)+'\n');return record

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();result=prepare(args.output)
    print(json.dumps(dict(project=str(args.output/'project.json'),copied_files=len(result['copied_inputs']))))
if __name__=='__main__':main()
