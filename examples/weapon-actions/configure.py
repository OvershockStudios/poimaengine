#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Author ordinary guarded UI controls into an independent prepared sample."""
import argparse,json,sys,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient

def operations():
    controls=[(510,'Mode','weapon-mode'),(511,'Cancel reload','weapon-cancel'),(512,'Holster','weapon-holster'),(513,'Equip','weapon-equip'),(514,'Reload','weapon-reload')]
    result=[]
    for index,(identity,text,action)in enumerate(controls):
        element=dict(parent=f'{500:032x}',name=text,kind='button',text=text,action=action,visible=True,enabled=True,
            layout=dict(position='absolute',left=dict(unit='dp',value=320),top=dict(unit='dp',value=145+index*46),width=dict(unit='dp',value=280),height=dict(unit='dp',value=42)),
            style=dict(background_color='#722f37',color='#f7e8eb',font_size=18,border_color='#722f37',border_width=1,focus=dict(border_color='#d46a7e')))
        result.append(dict(op='ui.element.set',id=f'{identity:032x}',element=element))
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in('binary','project','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();need_fresh=not args.output.exists()and args.output.parent.is_dir()
    if not need_fresh:parser.error('Fresh output with existing parent required.')
    manifest=json.loads(args.project.read_bytes());world=args.project.parent/manifest['entry']['world']
    if world.resolve().is_relative_to((ROOT/'docs/evidence/fixtures').resolve()):parser.error('Only configure an independent prepared copy.')
    args.output.mkdir();calls=[]
    with WorldClient.open(str(args.binary.resolve()),str(world.resolve()),trace_directory=str((args.output/'stdio').resolve()))as client:
        try:
            revision=client.call('world.inspect')['revision'];params=dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=operations())
            row=dict(method='world.transact',params=params);calls.append(row);row['result']=client.call(row['method'],params)
        finally:(args.output/'calls.json').write_text(json.dumps(calls,indent=2,allow_nan=False)+'\n')
if __name__=='__main__':main()
