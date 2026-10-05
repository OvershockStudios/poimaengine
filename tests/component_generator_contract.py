#!/usr/bin/env python3
"""Build real generated components and extract schemas without loading game code."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import uuid
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
HEADER = '#pragma warning disable CA2255 // Intentional metadata-only extraction trap.\nusing Poima; using System; using System.Runtime.CompilerServices;\n'
ATTR = '[GameplayComponent("11111111111111111111111111111111")] '
FIELD = '[GameplayField("00000000000000000000000000000001")] public int Value;'


def fingerprint(schema):
    text = 'poima.component.v1\n' + schema['id'] + '\n1\n'
    for field in sorted(schema['fields'], key=lambda f: f['id']):
        kind, value = field['kind'], field['default']
        if kind == 'entity':
            cell = struct.pack('<QQ', int(value[:16], 16), int(value[16:], 16))
        else:
            if kind.startswith('float') and value == 0:
                value = 0.0
            cell = struct.pack({'int32':'<i','int64':'<q','float32':'<f','float64':'<d'}[kind],
                               int(value) if kind.startswith('int') else value).ljust(16, b'\0')
        text += f"{field['id']}:{kind}:{cell.hex()}\n"
    return hashlib.sha256(text.encode('ascii')).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dotnet', type=Path, required=True)
    parser.add_argument('--extractor', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    run = args.output.resolve()/uuid.uuid4().hex
    run.mkdir(parents=True)
    env = dict(os.environ, DOTNET_GENERATE_ASPNET_CERTIFICATE='false', DOTNET_CLI_TELEMETRY_OPTOUT='1')
    evidence = dict(passed=False, checks=[], tool_sha256=hashlib.sha256(args.extractor.read_bytes()).hexdigest())
    def compile_case(name, source, good):
        directory = run/name; directory.mkdir()
        project = directory/'Probe.csproj'
        project.write_text(f'''<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net10.0</TargetFramework><LangVersion>14.0</LangVersion><AllowUnsafeBlocks>true</AllowUnsafeBlocks><ImplicitUsings>enable</ImplicitUsings><Nullable>enable</Nullable><TreatWarningsAsErrors>true</TreatWarningsAsErrors></PropertyGroup>
<ItemGroup><ProjectReference Include="{escape(str(ROOT/'managed/Poima.Gameplay/Poima.Gameplay.csproj'))}" /></ItemGroup>
<Import Project="{escape(str(ROOT/'managed/Poima.Components.targets'))}" /></Project>''')
        (directory/'Probe.cs').write_text(HEADER+source)
        result = subprocess.run([str(args.dotnet.resolve()),'build',str(project),'-c','Release','-m:1','-o',str(directory/'out')],
                                env=env,capture_output=True,text=True,timeout=120)
        (directory/'build.log').write_text(result.stdout+result.stderr)
        if not good:
            assert result.returncode != 0 and 'error POIMA001:' in result.stdout, (name,result.stdout,result.stderr)
            evidence['checks'].append('Rejected '+name);return None
        assert result.returncode == 0, (name,result.stdout,result.stderr)
        manifest = directory/'generated.poima-components.json'
        subprocess.run([str(args.dotnet.resolve()),str(args.extractor.resolve()),'--components',str(directory/'out/Probe.dll'),str(manifest)],
                       env=env,check=True,timeout=30)
        data=json.loads(manifest.read_text())
        for schema in data['schemas']:
            assert fingerprint(schema)==schema['fingerprint'], schema
        return data
    try:
        base=compile_case('valid',ATTR+'public partial struct Stats { '+FIELD+' }\n'+
            'public static class Trap { [ModuleInitializer] public static void Init() => throw new Exception("Game code must not execute during metadata extraction"); }',True)
        evidence['checks'].append('Actual PE metadata extraction succeeds with a throwing module initializer; no game code executes')
        labels=compile_case('labels',ATTR.replace(')]',',Name="Renamed")]')+'public partial struct Stats { '+FIELD.replace(')]',',Name="Display",Unit="units")]')+' }',True)
        assert base['schemas'][0]['fingerprint']==labels['schemas'][0]['fingerprint']
        evidence['checks'].append('Independent Python wire/hash oracle agrees; labels and units do not change compatibility')
        empty=compile_case('empty','public sealed class NoComponents {}',True)
        assert empty==dict(format='poima.components',version=1,schemas=[])
        evidence['checks'].append('No components produces a valid empty import manifest')
        invalid={
            'not_partial':ATTR+'public struct Stats { '+FIELD+' }',
            'managed_field':ATTR+'public partial struct Stats { [GameplayField("00000000000000000000000000000001")] public string Value; }',
            'hidden_state':ATTR+'public partial struct Stats { '+FIELD+' private int hidden; }',
            'auto_property':ATTR+'public partial struct Stats { '+FIELD+' public int Hidden {get;set;} }',
            'duplicate_id':ATTR+'public partial struct Stats { '+FIELD+' '+FIELD.replace('Value','Other')+' }',
            'duplicate_name':ATTR+'public partial struct Stats { '+FIELD.replace(')]',',Name="Same")]')+' '+FIELD.replace('00001','00002').replace('Value','Other').replace(')]',',Name="Same")]')+' }',
            'zero_type_id':ATTR.replace('1','0')+'public partial struct Stats { '+FIELD+' }',
            'entity_default':ATTR+'public partial struct Stats { [GameplayField("00000000000000000000000000000001",Default="00000000000000000000000000000001")] public EntityId Value; }',
            'nonfinite_default':ATTR+'public partial struct Stats { [GameplayField("00000000000000000000000000000001",Default="NaN")] public float Value; }',
            'noncanonical_int64':ATTR+'public partial struct Stats { [GameplayField("00000000000000000000000000000001",Default="01")] public long Value; }',
            'explicit_overlap':'[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Explicit)] '+ATTR+'public partial struct Stats { [System.Runtime.InteropServices.FieldOffset(0)] '+FIELD+' }',
            'nested':'public class Outer { '+ATTR+'public partial struct Stats { '+FIELD+' } }',
            'record_struct':ATTR+'public partial record struct Stats { '+FIELD+' }',
        }
        for name,source in invalid.items():compile_case(name,source,False)
        evidence['passed']=True
    finally:
        (run/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
        print(run/'evidence.json',flush=True)


if __name__=='__main__':main()
