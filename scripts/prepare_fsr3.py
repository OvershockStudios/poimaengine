#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare an optional pinned, upscaler-only FSR Vulkan dependency; never a default build step."""
import argparse,hashlib,json,os,pathlib,shutil,subprocess,time
PIN='c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'
URL='https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def require(ok,message):
    if not ok:raise RuntimeError(message)
def inventory(source):
    return {p.relative_to(source).as_posix():digest(p) for p in sorted((source/'sdk').rglob('*')) if p.is_file()}

CALLBACK='fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h'
CALLBACK_SHA='875c2b8d8df93716fb46271886c768a9ef40c5ccc11ade39397952b4a4e54321'
def shader_patch(source):
    raw=(source/'sdk/include/FidelityFX/gpu'/CALLBACK).read_bytes()
    require(hashlib.sha256(raw).hexdigest()==CALLBACK_SHA,'Unexpected GLSL callback source; refusing patch')
    before=b'binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8)'
    require(raw.count(before)==1,'Unexpected GLSL luma-history patch site')
    patched=raw.replace(before,b'binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba16f)')
    return patched,{'patch':'luma-history-rgba16f-v1','original_sha256':CALLBACK_SHA,
        'patched_sha256':hashlib.sha256(patched).hexdigest()}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--verify-only',action='store_true');p.add_argument('--git',default='git');a=p.parse_args();source=a.source.resolve();out=a.output.resolve()
    if a.verify_only:
        require(subprocess.check_output([a.git,'-C',str(source),'rev-parse','HEAD'],text=True).strip()==PIN,'FSR checkout must match pin')
        require(not subprocess.check_output([a.git,'-C',str(source),'status','--porcelain','--','sdk'],text=True).strip(),'FSR SDK tree must be unmodified')
        m=json.loads((out/'manifest.json').read_text());require(m['revision']==PIN and m['passed'],'Wrong FSR manifest')
        _,patch=shader_patch(source);require(m.get('shader_patch')==patch,'FSR shader cache lacks the required luma-history format correction; regenerate it')
        require(m.get('prepare_script_sha256')==digest(pathlib.Path(__file__)),'FSR preparation recipe changed; regenerate shader cache')
        for n,h in m['source_sha256'].items():require(digest(source/n)==h,'Changed FSR input: '+n)
        for n,h in m['shader_sha256'].items():require(digest(out/n)==h,'Changed FSR shader: '+n)
        require(len([n for n in m['shader_sha256']if n.endswith('_permutations.h')])==40,'Incomplete FSR shader set')
        print('Pinned FSR3.1.4 source and shader cache verified');return
    require(os.name=='nt','Shader preparation uses official Windows compiler binaries; run with Windows Python. Verification is portable.')
    if not source.exists():
        source.parent.mkdir(parents=True,exist_ok=True)
        subprocess.run([a.git,'clone','--filter=blob:none','--no-checkout','--depth','1','--branch','v1.1.4',URL,str(source)],check=True)
        # Pin working-tree bytes before sparse checkout materializes SDK sources.
        # Existing checkouts are validated, never silently normalized.
        subprocess.run([a.git,'-C',str(source),'config','core.autocrlf','false'],check=True)
        paths=['/LICENSE.txt','/sdk/LICENSE.txt','/sdk/include/FidelityFX/host/','/sdk/include/FidelityFX/gpu/*.h','/sdk/include/FidelityFX/gpu/fsr3upscaler/','/sdk/include/FidelityFX/gpu/fsr1/','/sdk/include/FidelityFX/gpu/spd/','/sdk/src/components/fsr3upscaler/','/sdk/src/backends/shared/','/sdk/src/shared/','/sdk/src/backends/vk/ffx_vk.cpp','/sdk/src/backends/vk/shaders/fsr3upscaler/','/sdk/tools/binary_store/FidelityFX_SC.exe','/sdk/tools/binary_store/glslangValidator.exe']
        subprocess.run([a.git,'-C',str(source),'sparse-checkout','set','--no-cone',*paths],check=True);subprocess.run([a.git,'-C',str(source),'checkout','--detach',PIN],check=True)
    require(subprocess.check_output([a.git,'-C',str(source),'rev-parse','HEAD'],text=True).strip()==PIN,'FSR checkout must match pin')
    require(not subprocess.check_output([a.git,'-C',str(source),'status','--porcelain'],text=True).strip(),'FSR checkout must be unmodified')
    out.mkdir(parents=True,exist_ok=True);before=inventory(source);sdk=source/'sdk';runs=[];start=time.monotonic()
    patched,patch=shader_patch(source)
    includes=out/'patched-gpu-include'
    shutil.copytree(sdk/'include/FidelityFX/gpu',includes,dirs_exist_ok=True)
    (includes/CALLBACK).write_bytes(patched)
    base=['-reflection','-deps=gcc','-num-threads=1','-compiler=glslang','-e','CS','--target-env','vulkan1.2','-S','comp','-Os','-DFFX_GLSL=1','-DFFX_GPU=1']
    for key,value in [('UPSAMPLE_SAMPLERS_USE_DATA_HALF',0),('ACCUMULATE_SAMPLERS_USE_DATA_HALF',0),('REPROJECT_SAMPLERS_USE_DATA_HALF',1),('POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF',0),('UPSAMPLE_USE_LANCZOS_TYPE',2)]:base.append('-DFFX_FSR3UPSCALER_OPTION_'+key+'='+str(value))
    for key in ['REPROJECT_USE_LANCZOS_TYPE','HDR_COLOR_INPUT','LOW_RESOLUTION_MOTION_VECTORS','JITTERED_MOTION_VECTORS','INVERTED_DEPTH','APPLY_SHARPENING']:base.append('-DFFX_FSR3UPSCALER_OPTION_'+key+'={0,1}')
    for shader in sorted((sdk/'src/backends/vk/shaders/fsr3upscaler').glob('*.glsl')):
        for suffix,half in [('',0),('_wave64',0),('_16bit',1),('_wave64_16bit',1)]:
            args=[str(sdk/'tools/binary_store/FidelityFX_SC.exe'),*base,'-DFFX_HALF='+str(half),'-glslangexe='+str(sdk/'tools/binary_store/glslangValidator.exe'),'-name='+shader.stem+suffix,'-I'+str(includes),'-output='+str(out),str(shader)]
            t=time.monotonic();r=subprocess.run(args,capture_output=True,text=True,timeout=180,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS);runs.append({'shader':shader.name,'variant':suffix,'exit_code':r.returncode,'seconds':time.monotonic()-t,'stdout':r.stdout,'stderr':r.stderr});(out/'prepare-log.json').write_text(json.dumps(runs,indent=2)+'\n');require(r.returncode==0,'FSR shader failed: '+shader.name+suffix);print(shader.stem+suffix,flush=True)
    require(inventory(source)==before,'FSR inputs changed during compilation');require(len(runs)==40,'Expected ten FSR passes and four families each')
    m={'passed':True,'revision':PIN,'shader_patch':patch,'prepare_script_sha256':digest(pathlib.Path(__file__)),'seconds':time.monotonic()-start,'source_sha256':before,'shader_sha256':{p.name:digest(p)for p in sorted(out.glob('*.h'))}};(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
if __name__=='__main__':main()
