"""Collection Room save-evolution fixture helper. Inputs are actual host-inspected target metadata/content."""
import argparse,hashlib,json
from pathlib import Path

def canonical(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False)
def digest(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--baseline',type=Path,required=True,help='Preserved source baseline containing manifest.json')
    p.add_argument('--mapping',type=Path,required=True)
    p.add_argument('--backend',choices=['coreclr','native_aot'],required=True)
    p.add_argument('--target-metadata',type=Path,required=True,help='Gameplay.inspect object, or native-gameplay.json descriptor')
    p.add_argument('--target-content-sha256',required=True,help='Actual frozen target content digest from host')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();baseline=a.baseline.resolve()
    m=json.loads((baseline/'manifest.json').read_text())
    for row in m['files']:
        b=(baseline/row['path']).read_bytes()
        if len(b)!=row['bytes'] or hashlib.sha256(b).hexdigest()!=row['sha256']:
            raise ValueError('Original baseline changed: '+row['path'])
    old=m['identities']['native' if a.backend=='native_aot' else 'coreclr']
    new=json.loads(a.target_metadata.read_text());schema=new['schema']
    if isinstance(schema,str):schema=json.loads(schema)
    if schema['identity']!=old['schema']['identity'] or new['type']!=old['type']:
        raise ValueError('Target identity/type differs')
    mapping=json.loads(a.mapping.read_text())
    wanted={x['id']:x['name'] for x in mapping['legacy_global_ids']}
    wanted[mapping['renamed_field']['id']]='RecoveredCount'
    wanted[mapping['new_field']['id']]='UpgradeSteps'
    if {x['id']:x['name'] for x in schema['persistent']['fields']}!=wanted:
        raise ValueError('Actual target persistent metadata differs from prepared mapping')
    source={'world_id':old['world_id'],'content_sha256':old['content_sha256'],'backend':a.backend,
            'module_identity':old['schema']['identity'],'type':old['type'],
            'image_sha256':old['assembly_sha256'],'schema_sha256':digest(old['schema'])}
    image=new.get('assembly_sha256',new.get('library_sha256'))
    if image is None and 'library' in new:
        matches=[row for row in new['files'] if row['path']==new['library']]
        if len(matches)!=1:raise ValueError('Target library inventory absent/ambiguous')
        row=matches[0];image=row['sha256'];binary=a.target_metadata.parent/new['library']
        if hashlib.sha256(binary.read_bytes()).hexdigest()!=image:raise ValueError('Target library hash differs')
    if image is None:raise ValueError('Target image digest absent')
    target=dict(source,content_sha256=a.target_content_sha256,image_sha256=image,schema_sha256=digest(schema))
    for obj in (source,target):
        for key in ('content_sha256','image_sha256','schema_sha256'):
            if len(obj[key])!=64 or any(c not in '0123456789abcdef' for c in obj[key]):raise ValueError('Invalid digest: '+key)
    plan={'format':'poima.save-upgrade','version':1,'id':digest({'source':source,'target':target})[:32],
          'source':source,'target':target,'global':mapping['global'],
          'legacy_global_ids':mapping['legacy_global_ids'],'components':[]}
    raw=json.dumps(plan,indent=2)+'\n';a.output.write_text(raw)
    print(json.dumps({'plan':str(a.output),'expected_sha256':hashlib.sha256(raw.encode()).hexdigest()}))
if __name__=='__main__':main()
