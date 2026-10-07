#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the pinned SDK backend with its EffectContext alignment correction.

The upstream checkout stays pristine. Only this generated translation unit is
compiled. Reject any different source, even if replacement text still matches.
"""
import argparse
import hashlib
import json
from pathlib import Path

ORIGINAL_SHA256 = '9fa430949184fadb357e6949d7564cf0f8e4653ead683a99755a6f15c4a76589'

def digest(data):
    return hashlib.sha256(data).hexdigest()

def write_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = args.source.read_bytes()
    if digest(original) != ORIGINAL_SHA256:
        raise SystemExit('Unexpected FSR Vulkan backend source hash; refusing patch')
    text = original.decode('utf-8')
    changes = [
        ('pipelineArraySize + resourceArraySize + contextArraySize,',
         'pipelineArraySize + resourceArraySize + contextArraySize + alignof(BackendContext_VK::EffectContext) - 1,'),
        ('        // Map context array\n        backendContext->pEffectContexts',
         '        // Poima patch: EffectContext is alignas(32), unlike preceding 8-byte arrays.\n'
         '        pMem = reinterpret_cast<uint8_t*>(FFX_ALIGN_UP(reinterpret_cast<uintptr_t>(pMem), alignof(BackendContext_VK::EffectContext)));\n'
         '        // Map context array\n        backendContext->pEffectContexts'),
    ]
    for before, after in changes:
        if text.count(before) != 1:
            raise SystemExit('Unexpected FSR backend patch site; refusing patch')
        text = text.replace(before, after)
    patched = text.encode('utf-8')
    write_changed(args.output, patched)
    record = {'patch': 'effect-context-alignment-v1', 'original_sha256': digest(original),
              'patched_sha256': digest(patched), 'patch_script_sha256': digest(Path(__file__).read_bytes()),
              'changes': ['Reserve alignof(EffectContext)-1 scratch bytes',
                          'Align actual EffectContext array address to alignof(EffectContext)']}
    write_changed(args.output.with_suffix('.provenance.json'), (json.dumps(record, indent=2)+'\n').encode())

if __name__ == '__main__':
    main()
