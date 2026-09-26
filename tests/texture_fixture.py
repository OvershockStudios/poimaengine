"""Original textured quad and PNG fixtures; no downloaded art."""
# SPDX-License-Identifier: Apache-2.0
import json
import struct
import zlib
from gltf_fixture import glb


def png(width,height,pixels):
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    raw=b''.join(b'\0'+bytes(pixels[y*width*4:(y+1)*width*4]) for y in range(height))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',width,height,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b'')


def quad(images,material=None,sampler=None,uvs=None):
    uvs=uvs or [(0,1),(1,1),(1,0),(0,0)]
    vertices=[]
    for p,uv in zip([(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],uvs):vertices.extend((*p,0,0,1,*uv))
    blob=struct.pack('<32f6I',*vertices,0,1,2,0,2,3)
    doc={'asset':{'version':'2.0'},'buffers':[{'byteLength':len(blob)}],
         'bufferViews':[{'buffer':0,'byteLength':128,'byteStride':32},{'buffer':0,'byteOffset':128,'byteLength':24}],
         'accessors':[{'bufferView':0,'componentType':5126,'count':4,'type':'VEC3','min':[-1,-1,0],'max':[1,1,0]},
                      {'bufferView':0,'byteOffset':12,'componentType':5126,'count':4,'type':'VEC3'},
                      {'bufferView':0,'byteOffset':24,'componentType':5126,'count':4,'type':'VEC2'},
                      {'bufferView':1,'componentType':5125,'count':6,'type':'SCALAR'}],
         'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1,'TEXCOORD_0':2},'indices':3,'material':0}]}],
         'materials':[material or {'pbrMetallicRoughness':{'baseColorTexture':{'index':0}}}],
         'textures':[],'images':[],'samplers':[sampler or {'magFilter':9728,'minFilter':9728}],
         'nodes':[{'name':'Textured quad','mesh':0}],'scenes':[{'nodes':[0]}],'scene':0}
    for image in images:
        blob+=b'\0'*((-len(blob))%4);offset=len(blob);blob+=image
        doc['images'].append({'bufferView':len(doc['bufferViews']),'mimeType':'image/jpeg' if image.startswith(b'\xff\xd8') else 'image/png'})
        doc['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(image)})
        doc['textures'].append({'source':len(doc['images'])-1,'sampler':0})
    doc['buffers'][0]['byteLength']=len(blob)
    return doc,blob
