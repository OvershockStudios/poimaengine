"""Original analytic sphere fixture: no downloaded art or runtime Python code."""
# SPDX-License-Identifier: Apache-2.0
import json
import math
from pathlib import Path
import struct


def sphere(rings=24,segments=48):
    vertices=[];indices=[]
    for row in range(rings+1):
        theta=math.pi*row/rings
        for column in range(segments+1):
            phi=2*math.pi*column/segments
            p=(math.sin(theta)*math.cos(phi),math.cos(theta),math.sin(theta)*math.sin(phi))
            vertices.extend((*p,*p))
    for row in range(rings):
        for column in range(segments):
            a=row*(segments+1)+column;b=a+segments+1
            if row>0:indices.extend((a,a+1,b))
            if row<rings-1:indices.extend((a+1,b+1,b))
    vertex_bytes=struct.pack('<'+'f'*len(vertices),*vertices)
    blob=vertex_bytes+struct.pack('<'+'I'*len(indices),*indices)
    doc={'asset':{'version':'2.0','generator':'Poima original analytic test fixture'},'buffers':[{'byteLength':len(blob)}],
         'bufferViews':[{'buffer':0,'byteOffset':0,'byteLength':len(vertex_bytes),'byteStride':24,'target':34962},
                        {'buffer':0,'byteOffset':len(vertex_bytes),'byteLength':len(indices)*4,'target':34963}],
         'accessors':[{'bufferView':0,'componentType':5126,'count':len(vertices)//6,'type':'VEC3','min':[-1,-1,-1],'max':[1,1,1]},
                      {'bufferView':0,'byteOffset':12,'componentType':5126,'count':len(vertices)//6,'type':'VEC3'},
                      {'bufferView':1,'componentType':5125,'count':len(indices),'type':'SCALAR'}],
         'materials':[{'name':'Copper test','pbrMetallicRoughness':{'baseColorFactor':[.7,.25,.07,1],'metallicFactor':.7,'roughnessFactor':.25}}],
         'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1},'indices':2,'material':0}]}],
         'nodes':[{'name':'Assembly','children':[1]},{'name':'Sphere','mesh':0}],
         'scenes':[{'nodes':[0]}],'scene':0}
    return doc,blob


def glb(doc,blob):
    encoded=json.dumps(doc,separators=(',',':')).encode();encoded+=b' '*((-len(encoded))%4)
    binary=blob+b'\0'*((-len(blob))%4)
    return struct.pack('<III',0x46546c67,2,28+len(encoded)+len(binary))+struct.pack('<II',len(encoded),0x4e4f534a)+encoded+struct.pack('<II',len(binary),0x004e4942)+binary


def write_fixture(directory):
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=True)
    doc,blob=sphere();(directory/'sphere.glb').write_bytes(glb(doc,blob))
    doc['buffers'][0]['uri']='sphere.bin';(directory/'sphere.bin').write_bytes(blob);(directory/'sphere.gltf').write_text(json.dumps(doc))
    return directory/'sphere.glb',directory/'sphere.gltf'
