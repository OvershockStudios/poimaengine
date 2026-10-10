#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Convert selected original licensed sounds to the native spatial WAV profile.

Offline asset preparation only; no network requests or generative model.
Requires native libsndfile and libsamplerate (or explicit DLL paths). Originals
are copied unchanged and every input/output/library is recorded. Other library
builds may produce different cooked bytes; the launcher verifies the published
sound manifest rather than assuming cross-toolchain bit equality.
"""
import argparse
import ctypes as C
import ctypes.util
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import wave
import zipfile

PACKS = {
    'impact': dict(archive_sha256='029d734af1582474edf3a694d1b0cebc97c1c152f2f39fa34d4c2bafc5de77f8',
        source='https://kenney.nl/assets/impact-sounds',
        license_sha256='b49aa9c56b04528b95913de13e506a0f7c5e807b9925db9bfef86af1f91120db'),
    'interface': dict(archive_sha256='f2193d072726d6758a5f7871b2dcc54dcce0d5c35c6f0a62f92549b327c81232',
        source='https://kenney.nl/assets/interface-sounds',
        license_sha256='f7966c773bbed0eca6a9c75081c44a178b38eae112724dbb5fdfbd4192d118a9')}
SOUNDS = {
    'step-a': ('impact','Audio/footstep_concrete_000.ogg','d7267e183067757c92c169de2a379abea592cfca6b39bb2e8feea15221ad79fe'),
    'step-b': ('impact','Audio/footstep_concrete_001.ogg','507a75eb19b897087855a3be3cf11a12d7fa37d143ad535eeebe0b8031c7325d'),
    'pickup': ('interface','Audio/confirmation_001.ogg','063564703b6094d70718a3e787a55cc9141611e4ecd6b6637f8828f79b4a8c3a'),
    'relay': ('interface','Audio/confirmation_002.ogg','33b17a9a9a2397c62b285c52c33a907fdffb476909c99e42dde603f6a7a8b12c'),
    'denied': ('interface','Audio/error_001.ogg','46e67425d16339772e8d328fb36a49426c9467418686e11beeb71ff84b0f6433')}


def need(value, message):
    if not value:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class Info(C.Structure):
    _fields_ = [('frames',C.c_longlong),('samplerate',C.c_int),('channels',C.c_int),
                ('format',C.c_int),('sections',C.c_int),('seekable',C.c_int)]


class Conversion(C.Structure):
    _fields_ = [('data_in',C.POINTER(C.c_float)),('data_out',C.POINTER(C.c_float)),
                ('input_frames',C.c_long),('output_frames',C.c_long),
                ('input_frames_used',C.c_long),('output_frames_gen',C.c_long),
                ('end_of_input',C.c_int),('src_ratio',C.c_double)]


def library_file(lib):
    path = Path(lib._name)
    if path.is_file():
        return path.resolve()
    # Linux's loader may resolve a SONAME. Identify the loaded native file,
    # without invoking a shell or treating the version string as a binary hash.
    maps = Path('/proc/self/maps')
    if maps.is_file():
        for line in maps.read_text().splitlines():
            candidate = line.split()[-1]
            if candidate.startswith('/') and Path(candidate).name.startswith(path.name.split('.so')[0]+'.so'):
                return Path(candidate).resolve(strict=True)
    raise ValueError('Supply an absolute native library path to record its bytes.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('impact-archive','interface-archive','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--sndfile')
    p.add_argument('--samplerate')
    args = p.parse_args()
    need(not args.output.exists(),'Use a new output directory; failed preparations are retained.')
    sf = C.CDLL(args.sndfile or ctypes.util.find_library('sndfile') or 'sndfile')
    sr = C.CDLL(args.samplerate or ctypes.util.find_library('samplerate') or 'samplerate')
    sf.sf_version_string.restype=C.c_char_p; sr.src_get_version.restype=C.c_char_p
    sf.sf_open.argtypes=[C.c_char_p,C.c_int,C.POINTER(Info)];sf.sf_open.restype=C.c_void_p
    sf.sf_readf_float.argtypes=[C.c_void_p,C.POINTER(C.c_float),C.c_longlong];sf.sf_readf_float.restype=C.c_longlong
    sf.sf_close.argtypes=[C.c_void_p];sf.sf_close.restype=C.c_int
    sr.src_simple.argtypes=[C.POINTER(Conversion),C.c_int,C.c_int];sr.src_simple.restype=C.c_int
    libraries = dict(sndfile=dict(version=sf.sf_version_string().decode(),sha256=digest(library_file(sf).read_bytes())),
                     samplerate=dict(version=sr.src_get_version().decode(),sha256=digest(library_file(sr).read_bytes())))
    archives={}
    for name,path in (('impact',args.impact_archive),('interface',args.interface_archive)):
        need(path.is_file() and path.stat().st_size<=32*1024*1024,'Expected bounded original archive.')
        need(digest(path.read_bytes())==PACKS[name]['archive_sha256'],'Original archive differs: '+name)
        archives[name]=path
    args.output.mkdir(parents=True)
    manifest=dict(format='poima.relay-audio',version=1,creator='Kenney',license='CC0-1.0',
        procedure=dict(id='relay-audio-convert-v1',decoder='sf_readf_float',
            downmix='Arithmetic mean of original channels, rounded to float32',
            resampler='libsamplerate SRC_SINC_BEST_QUALITY, end_of_input=1, mono, ratio=48000/source_rate',
            encoding='48kHz mono PCM16; round-to-nearest-even(sample*32768), saturate to [-32768,32767]',
            script_sha256=digest(Path(__file__).read_bytes()),libraries=libraries),packs={},sounds={})
    for pack,path in archives.items():
        with zipfile.ZipFile(path) as z:
            need(len(z.namelist())==len(set(z.namelist())),'Duplicate archive names.')
            license_bytes=z.read('License.txt')
            need(digest(license_bytes)==PACKS[pack]['license_sha256'],'Original license differs.')
            folder=args.output/'sources'/pack;folder.mkdir(parents=True)
            (folder/'License.txt').write_bytes(license_bytes)
            manifest['packs'][pack]=dict(**PACKS[pack],license_path=f'sources/{pack}/License.txt',
                license_bytes=len(license_bytes))
            for name,(source_pack,member,pin) in SOUNDS.items():
                if source_pack!=pack:
                    continue
                original=z.read(member);need(len(original)<=1024*1024 and digest(original)==pin,'Original sound differs.')
                copied=folder/Path(member).name;copied.write_bytes(original)
                info=Info();handle=sf.sf_open(os.fsencode(copied.resolve()),0x10,C.byref(info))
                need(handle,'Native decoder could not open original sound.')
                try:
                    need(1<=info.channels<=2 and 1<=info.frames<=60*info.samplerate and
                         8000<=info.samplerate<=192000,'Source exceeds preparation profile.')
                    original_pcm=(C.c_float*(info.frames*info.channels))()
                    need(sf.sf_readf_float(handle,original_pcm,info.frames)==info.frames,'Decoder returned incomplete sound.')
                finally:
                    need(sf.sf_close(handle)==0,'Decoder close failed.')
                mono=(C.c_float*info.frames)()
                for i in range(info.frames):
                    value=sum(float(original_pcm[i*info.channels+c]) for c in range(info.channels))/info.channels
                    need(math.isfinite(value),'Nonfinite source audio.')
                    mono[i]=value
                capacity=math.ceil(info.frames*48000/info.samplerate)+64
                converted=(C.c_float*capacity)()
                conversion=Conversion(mono,converted,info.frames,capacity,0,0,1,48000/info.samplerate)
                need(sr.src_simple(C.byref(conversion),0,1)==0 and conversion.input_frames_used==info.frames,
                     'Native resampler failed or left input unused.')
                frames=conversion.output_frames_gen;need(1<=frames<=2880000,'Converted clip exceeds bounds.')
                pcm=bytearray();clamped=0
                for value in converted[:frames]:
                    need(math.isfinite(value),'Nonfinite resampler output.')
                    quantized=round(float(value)*32768);bounded=max(-32768,min(32767,quantized))
                    clamped+=bounded!=quantized;pcm+=struct.pack('<h',bounded)
                target=args.output/(name+'.wav')
                with wave.open(str(target),'wb') as wav:
                    wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(48000);wav.writeframes(pcm)
                manifest['sounds'][name]=dict(path=name+'.wav',bytes=target.stat().st_size,sha256=digest(target.read_bytes()),
                    frames=frames,channels=1,sample_rate=48000,clamped_samples=clamped,pack=pack,
                    original_path=copied.relative_to(args.output).as_posix(),original_sha256=pin,
                    original_bytes=len(original),original_archive_member=member,
                    original_frames=info.frames,original_channels=info.channels,original_sample_rate=info.samplerate)
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(prepared=True,sounds=len(manifest['sounds']),manifest_sha256=digest((args.output/'manifest.json').read_bytes()))))


if __name__=='__main__':
    main()
