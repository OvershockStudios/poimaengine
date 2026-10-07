#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real output-resolution UI composition after every reconstruction mode.
An opaque black UI backing owns every output pixel, permitting exact mode and
exposure comparisons independent of scene reconstruction quality.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from ui_capture import image, encode

MODES = ('none', 'fsr3_native', 'fsr3_quality', 'fsr3_balanced', 'fsr3_performance')
SUFFIXES = ('-baseline.bmp', '-ui.bmp', '-ui-exposure-zero.bmp', '-ui-exposure-high.bmp', '-baseline-zero.bmp')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--lighting-path', choices=('forward','deferred'), default='forward')
    args = parser.parse_args()
    require(not sys.flags.optimize, 'Shared BMP reader requires assertions')
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    destination = output/'reconstruction-ui.json'
    prefixes = {mode:output/('ui-'+mode) for mode in MODES}
    require(not destination.exists() and not any(Path(str(prefix)+suffix).exists() for prefix in prefixes.values() for suffix in SUFFIXES), 'Use a fresh output directory; prior evidence is preserved')
    binary = args.binary.resolve()
    inputs = [binary, Path(__file__).resolve(), Path(__file__).with_name('ui_capture.py').resolve(), Path(__file__).with_name('ui_capture_native.cpp').resolve()]
    original_hashes = {str(path):sha(path) for path in inputs}
    evidence = {'passed':False, 'quality_qualified':False, 'scope':'UI composition only; no scene reconstruction quality claim',
                'gpu_index':args.gpu, 'lighting_path':args.lighting_path, 'input_hashes_before':original_hashes, 'cases':[]}
    actual_gpu = None; baseline_ui = None
    try:
        for mode in MODES:
            prefix = prefixes[mode]; argument = str(prefix)
            if args.windows_interop:
                argument = subprocess.check_output(['wslpath','-w',argument],text=True).strip()
            row = {'mode':mode, 'passed':False}; evidence['cases'].append(row)
            command = [str(binary), argument, str(args.gpu), '1', mode, args.lighting_path]
            row['command'] = command
            try:
                result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=180)
            except subprocess.TimeoutExpired as error:
                row['timeout_seconds'] = error.timeout
                row['stdout'] = error.stdout.decode('utf-8',errors='replace') if isinstance(error.stdout,bytes) else error.stdout
                row['stderr'] = error.stderr.decode('utf-8',errors='replace') if isinstance(error.stderr,bytes) else error.stderr
                raise
            row.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr)
            require(result.returncode == 0, 'Native UI fixture failed: '+mode)
            response = json.loads(result.stdout); row['response'] = response
            require(response['requested_reconstruction'] == mode, 'Requested mode differs')
            require(response['requested_lighting_path'] == args.lighting_path, 'Requested lighting path differs')
            for name in ('baseline','ui','ui_zero','ui_high','baseline_zero'):
                report = response[name]
                require(report['success'] and report['hardware'] and report['capture_written'] and report['validation_errors'] == 0, 'Capture failed: '+name)
                require(report['samples'] == 1 and (report['width'],report['height']) == (320,240), 'Capture extent/sample count differs')
                actual_gpu = actual_gpu or report['gpu']
                require(report['gpu'] == actual_gpu, 'Actual GPU changed between captures')
                require(report['deferred'] == (args.lighting_path == 'deferred'), 'Actual lighting path differs')
                reconstruction = report['reconstruction']
                require(reconstruction['active'] == (mode != 'none') and reconstruction['mode'] == mode, 'Requested reconstruction was not used')
                if mode != 'none':
                    ratio = {'fsr3_native':1,'fsr3_quality':1.5,'fsr3_balanced':1.7,'fsr3_performance':2}[mode]
                    require(reconstruction['render_extent'] == [int(320/ratio),int(240/ratio)] and reconstruction['output_extent'] == [320,240], 'Reconstruction scale differs')
            captures = {suffix:image(Path(str(prefix)+suffix)) for suffix in SUFFIXES}
            row['image_hashes'] = {suffix:digest for suffix,(_,digest) in captures.items()}
            pixel = captures['-ui.bmp'][0]
            alpha = 128/255
            grey = encode((((.5+.055)/1.055)**2.4)*alpha)
            checks = {
                'opaque':((40,40),(255,0,0)),
                'linear_overlap':((90,60),(encode(1-alpha),encode(alpha),0)),
                'linear_alpha':((140,80),(0,encode(alpha),0)),
                'clip_inside':((200,40),(0,0,255)),
                'clip_left':((185,40),(0,0,0)),
                'clip_bottom':((210,70),(0,0,0)),
                'transformed':((40,170),(255,255,0)),
                'transform_outside':((65,170),(0,0,0)),
                'premultiplied_texture':((120,175),(grey,grey,grey)),
                'background':((300,220),(0,0,0))}
            row['probes'] = {}
            for name,(point,expected) in checks.items():
                value = pixel(*point); row['probes'][name] = {'pixel':point,'actual':value,'expected':expected}
                require(all(abs(a-b)<=2 for a,b in zip(value,expected)), 'Original composition oracle failed: '+name)
            background = captures['-baseline.bmp'][0](160,120)
            dark = captures['-baseline-zero.bmp'][0](160,120)
            require(background != dark and dark == (0,0,0), 'Non-UI exposure control ineffective')
            reference = [pixel(x,y) for y in range(240) for x in range(320)]
            if baseline_ui is None: baseline_ui = reference
            for y in range(240):
                for x in range(320):
                    expected = baseline_ui[y*320+x]
                    for suffix in ('-ui.bmp','-ui-exposure-zero.bmp','-ui-exposure-high.bmp'):
                        value = captures[suffix][0](x,y)
                        if value != expected:
                            row['first_mismatch'] = {'image':suffix,'x':x,'y':y,'expected_none':expected,'actual':value}
                            raise RuntimeError('UI-owned output pixel changed with reconstruction/exposure')
            row.update(passed=True, compared_pixels_per_image=320*240, exposure_values=[0,1,64], full_image_exact_equal_to_none=True)
        evidence['input_hashes_after'] = {str(path):sha(path) for path in inputs}
        require(evidence['input_hashes_after'] == original_hashes, 'Qualification inputs changed during execution')
        evidence['actual_gpu'] = actual_gpu; evidence['passed'] = True
    except Exception as error:
        evidence['error'] = str(error)
        raise
    finally:
        destination.write_text(json.dumps(evidence,indent=2)+'\n')
    print(json.dumps(evidence,indent=2))


if __name__ == '__main__':
    main()
