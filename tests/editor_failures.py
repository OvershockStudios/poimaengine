#!/usr/bin/env python3
"""Editor argument/file safety failures must precede project writes and GPU use."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import subprocess
import uuid

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('binary',type=Path)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--windows-interop',action='store_true')
a=p.parse_args();run=a.output/uuid.uuid4().hex;run.mkdir(parents=True)
world=run/'world.json';script=run/'actions.json';script.write_text('{"actions":[]}\n')
def native(path):
    return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if a.windows_interop else str(path.resolve())
record={'passed':False,'cases':[]}
def reject(name,args):
    before={str(x):x.read_bytes() for x in run.rglob('*') if x.is_file()}
    result=subprocess.run([str(a.binary.resolve()),'editor',native(world),'--frames','1',*args],capture_output=True,text=True,timeout=20)
    record['cases'].append({'name':name,'exit_code':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
    assert result.returncode!=0,result.stdout+result.stderr
    assert json.loads(result.stdout)['status']=='error',result.stdout
    assert not world.exists(),name
    for path,data in before.items():assert Path(path).read_bytes()==data,name
try:
    reject('report aliases world',['--report',native(world)])
    reject('capture aliases world',['--capture',native(world)])
    for suffix in ['.lock','.previous','.pending','.previous.pending']:
        reject('report aliases world'+suffix,['--report',native(Path(str(world)+suffix))])
    reject('capture aliases script',['--script',native(script),'--capture',native(script)])
    reject('report aliases script',['--script',native(script),'--report',native(script)])
    same=run/'same.out'
    reject('report aliases capture',['--capture',native(same),'--report',native(same)])
    existing=run/'existing.out';existing.write_text('Keep this file.\n')
    reject('existing report',['--report',native(existing)])
    reject('existing capture',['--capture',native(existing)])
    huge=run/'huge.json';huge.write_text(' '*1048577)
    reject('oversized script',['--script',native(huge)])
    malformed=run/'malformed.json';malformed.write_text('{')
    reject('malformed script',['--script',native(malformed)])
    record['passed']=True
finally:
    (run/'evidence.json').write_text(json.dumps(record,indent=2)+'\n');print(run/'evidence.json')
