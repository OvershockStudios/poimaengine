"""Original two-joint ribbon and independently specified glTF curves."""
# SPDX-License-Identifier: Apache-2.0
import struct

def ribbon(*, normals=True, integer_weights=False, cubic=False):
    blob=bytearray();views=[];accessors=[]
    def accessor(rows,kind,fmt='f',normalized=False):
        while len(blob)%4:blob.append(0)
        start=len(blob);flat=[x for row in rows for x in row]
        blob.extend(struct.pack('<'+fmt*len(flat),*flat))
        views.append({'buffer':0,'byteOffset':start,'byteLength':len(blob)-start})
        result={'bufferView':len(views)-1,'componentType':{'f':5126,'B':5121,'H':5123}[fmt],'count':len(rows),'type':kind}
        if normalized:result['normalized']=True
        accessors.append(result);return len(accessors)-1
    positions=[(-.5,0,0),(.5,0,0),(-.5,1,0),(.5,1,0),(-.5,2,0),(.5,2,0)]
    p=accessor(positions,'VEC3');accessors[p].update(min=[-.5,0,0],max=[.5,2,0])
    attrs={'POSITION':p,'TEXCOORD_0':accessor([(x+.5,y/2) for x,y,z in positions],'VEC2'),
           'JOINTS_0':accessor([(0,1,0,0)]*6,'VEC4','B')}
    weights=[(1,0,0,0)]*2+[(.5,.5,0,0)]*2+[(0,1,0,0)]*2
    if integer_weights:weights=[(255,0,0,0)]*2+[(128,127,0,0)]*2+[(0,255,0,0)]*2
    attrs['WEIGHTS_0']=accessor(weights,'VEC4','B' if integer_weights else 'f',integer_weights)
    if normals:attrs['NORMAL']=accessor([(0,0,1)]*6,'VEC3')
    indices=accessor([(i,) for i in [0,1,2,1,3,2,2,3,4,3,5,4]],'SCALAR','H')
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];child=identity.copy();child[13]=-1
    ibm=accessor([identity,child],'MAT4')
    times=accessor([(0,),(2,)],'SCALAR');accessors[times].update(min=[0],max=[2])
    samples=[(0,1,0),(2,1,0)]
    if cubic:samples=[(0,0,0),(0,1,0),(2,0,0),(0,0,0),(2,1,0),(0,0,0)]
    values=accessor(samples,'VEC3')
    doc={'asset':{'version':'2.0','generator':'Poima original two-joint analytic ribbon'},'buffers':[{'byteLength':len(blob)}],
         'bufferViews':views,'accessors':accessors,
         'meshes':[{'primitives':[{'attributes':attrs,'indices':indices,'material':0}]}],
         'materials':[{'pbrMetallicRoughness':{'baseColorFactor':[.2,.55,.85,1],'metallicFactor':.1,'roughnessFactor':.4},'doubleSided':True}],
         # Child appears before its parent; mesh-node offset should cancel when skinned.
         'nodes':[{'name':'Ribbon','mesh':0,'skin':0,'translation':[7,0,0]},{'name':'Tip','translation':[0,1,0]}, {'name':'Root','children':[1]}],
         'skins':[{'name':'Ribbon rig','skeleton':2,'joints':[2,1],'inverseBindMatrices':ibm}],
         'animations':[{'name':'Bend','samplers':[{'input':times,'output':values,'interpolation':'CUBICSPLINE' if cubic else 'LINEAR'}], 'channels':[{'sampler':0,'target':{'node':1,'path':'translation'}}]}],
         'scenes':[{'nodes':[0,2]}],'scene':0}
    return doc,bytes(blob)
