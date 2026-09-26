#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate original sample art offline. This is not a native material-graph API."""
from pathlib import Path
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tests'))
from gltf_fixture import sphere,glb
from texture_fixture import png

root=Path(__file__).resolve().parent
doc,old=sphere();vertex_count=doc['accessors'][0]['count'];old_vertex_bytes=vertex_count*24
vertices=[]
for i in range(vertex_count):
    values=struct.unpack_from('<6f',old,i*24);vertices.extend((*values,(i%49)/48,(i//49)/24))
blob=struct.pack('<'+'f'*len(vertices),*vertices)+old[old_vertex_bytes:]
doc['bufferViews'][0].update(byteLength=vertex_count*32,byteStride=32);doc['bufferViews'][1]['byteOffset']=vertex_count*32
doc['accessors'].append({'bufferView':0,'byteOffset':24,'componentType':5126,'count':vertex_count,'type':'VEC2'})
doc['meshes'][0]['primitives'][0]['attributes']['TEXCOORD_0']=3
doc['textures']=[];doc['images']=[];doc['samplers']=[{'minFilter':9987,'magFilter':9729}]
color=[];mr=[];ao=[]
for y in range(256):
    for x in range(512):
        # Alternating tile layout, fine surface variation, seams and edge wear.
        row=y//32;xx=(x+16*(row%2))%32;yy=y%32;edge=min(xx,31-xx,yy,31-yy)
        noise=((x*1973+y*9277+89173)*26699)&0xffffffff;noise=((noise^(noise>>13))*1274126177)&0xffffffff
        fine=(noise%25)-12;seam=edge<1;wear=edge<3
        if seam:rgb=(32,39,42)
        elif wear:rgb=(144+fine,120+fine,81+fine)
        else:rgb=(78+fine,112+fine,111+fine)
        color.extend((*rgb,255));mr.extend((255,210 if seam else 160 if wear else 95+noise%65,190 if wear else 255,255));ao.extend(([90]*3 if seam else [255]*3)+[255])
for values in [color,mr,ao]:
    data=png(512,256,values);blob+=b'\0'*((-len(blob))%4)
    doc['bufferViews'].append({'buffer':0,'byteOffset':len(blob),'byteLength':len(data)});blob+=data
    doc['images'].append({'bufferView':len(doc['bufferViews'])-1,'mimeType':'image/png'});doc['textures'].append({'source':len(doc['images'])-1,'sampler':0})
doc['buffers'][0]['byteLength']=len(blob)
doc['materials']=[{'name':'Original tiled surface','pbrMetallicRoughness':{'baseColorFactor':[1,1,1,1],'baseColorTexture':{'index':0},'metallicRoughnessTexture':{'index':1},'metallicFactor':0,'roughnessFactor':1},'occlusionTexture':{'index':2,'strength':.8}}]
(root/'textured-sphere.glb').write_bytes(glb(doc,blob))
