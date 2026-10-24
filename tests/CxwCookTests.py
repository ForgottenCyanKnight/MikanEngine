"""Verify CPU-only CXW conversion publishes valid standalone engine voxmesh files."""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib
from CxwLoaderTests import world, region, RED


def checksum(words):
    h=2166136261
    for word in words:
        for byte in word.to_bytes(4,"little"):
            h=((h^byte)*16777619)&0xFFFFFFFF
    return h


def decode(path):
    blob=path.read_bytes()
    header=struct.unpack_from("<12I",blob)
    assert header[0]==0x5A4D5856 and header[1]==1 and len(blob)==48+header[10]
    raw=zlib.decompress(blob[48:])
    assert len(raw)==header[8]
    leaves=[]
    words=list(struct.unpack("<"+"I"*(len(raw)//4),raw))
    if words[1]==9:
        assert words[0]==0x534D5856 and checksum(words[:-1])==words[-1]
        at=4
        for _ in range(words[3]):
            length=words[at];at+=1
            name=struct.pack("<"+"I"*((length+3)//4),*words[at:at+(length+3)//4])[:length].decode("utf-8")
            at+=(length+3)//4
            relative=pathlib.Path(name)
            assert not relative.is_absolute() and ".." not in relative.parts
            leaves.extend(decode(path.parent/relative))
        assert at==len(words)-1
        return leaves

    def parse(w):
        assert w[0]==0x534D5856 and checksum(w[:-1])==w[-1]
        if w[1]==4:
            assert w[4]==len(w)-6
            parse(w[5:-1])
        elif w[1]==3:
            assert w[3]==w[4]
            at=5
            for _ in range(w[3]):
                n=w[at];at+=1;parse(w[at:at+n]);at+=n
            assert at+w[4]*13==len(w)-1
        else:
            assert w[1] in (5,8)
            n,attribute_words=w[4:6]
            plane_count=w[18]
            base=19+2*plane_count
            attributes=w[base+n:-1]
            assert len(attributes)==attribute_words and sum(w[12:18])==n
            palette=(attributes[1]-attributes[0])//2
            colors=set()
            for q in range(n):
                descriptor=attributes[attributes[1]+q]
                value=descriptor&0x3FFFFFFF
                if descriptor&0x80000000:
                    wide=bool(descriptor&0x40000000)
                    if wide:
                        assert w[1]==8 and value%2==0
                    packed=w[base+q]
                    cells=(((packed>>16)&255)+1)*((packed>>24)+1)
                    ids=[]
                    for k in range(cells):
                        byte=value+k*(2 if wide else 1)
                        ids.append((attributes[attributes[2]+byte//4]>>((byte%4)*8))&(65535 if wide else 255))
                else:
                    ids=[value]
                for idx in ids:
                    assert idx<palette
                    colors.add(attributes[attributes[0]+idx*2])
            materials={attributes[attributes[0]+idx*2+1] for idx in range(palette)}
            leaves.append({"quads":n,"palette":palette,"colors":colors,"voxels":w[3],"materials":materials})

    parse(words)
    return leaves


def main():
    exe=pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="cxw-cook-") as tmp:
        tmp=pathlib.Path(tmp)

        def convert(name,data,workers=4,uniform=False,clear_glass=False,shard_mib=256):
            source=tmp/(name+".cxw")
            if not source.exists():source.write_bytes(data)
            output=tmp/(name+f"-{workers}"+("-uniform" if uniform else "")+".voxmesh")
            environment=dict(os.environ,MIKAN_CXW_WORKERS=str(workers),MIKAN_CXW_CLEAR_GLASS="1" if clear_glass else "0",MIKAN_CXW_SHARD_MIB=str(shard_mib))
            p=subprocess.run([str(exe),"--convert",str(source),str(output)]+(["--uniform"] if uniform else []),capture_output=True,text=True,errors="replace",env=environment)
            assert p.returncode==0,(p.stdout,p.stderr)
            return output,decode(output)

        slab=world([((0,0,0),region({(i%20,i//20,3):0x8000|i for i in range(300)}))])
        _,leaves=convert("r16",slab)
        assert len(leaves)==1 and leaves[0]["quads"]==6 and len(leaves[0]["colors"])==300 and leaves[0]["palette"]==300
        _,leaves=convert("r16",slab,uniform=True)
        assert leaves[0]["quads"]==670 and len(leaves[0]["colors"])==300
        tiled=world([((x,y,z),region({(0,0,0):RED|(x+2*y+4*z)})) for z in range(2) for y in range(2) for x in range(2)])
        _,leaves=convert("eight-sectors",tiled)
        assert len(leaves)==1 and leaves[0]["voxels"]==8 and len(leaves[0]["colors"])==8
        several=world([((x,0,0),region({(0,0,0):RED|x})) for x in (0,2,4,6)])
        a,_=convert("deterministic",several,workers=1)
        b,_=convert("deterministic",several,workers=4)
        assert a.read_bytes()==b.read_bytes(),"worker count changed cooked bytes"
        _,leaves=convert("clear-glass",world([((0,0,0),region({(0,0,0):1}))]),clear_glass=True)
        assert leaves[0]["colors"]=={0xffffff00} and leaves[0]["materials"]=={8} and leaves[0]["quads"]==6
        _,leaves=convert("glass-wall",world([((0,0,0),region({(0,0,0):1,(1,0,0):RED}))]),clear_glass=True)
        assert leaves[0]["materials"]=={0,8} and leaves[0]["colors"]=={0xffffff00,0xff00}
        scattered=region({(x*3,y*3,z*3):RED for z in range(20) for y in range(20) for x in range(20)})
        source=world([((x,0,0),scattered) for x in (0,2,4,6)])
        output,leaves=convert("sharded",source,shard_mib=1)
        assert len(leaves)==4 and sum(leaf["voxels"] for leaf in leaves)==32000 and sum(leaf["quads"] for leaf in leaves)==192000
        root=zlib.decompress(output.read_bytes()[48:]);assert struct.unpack_from("<4I",root)[1:]==(9,2,2)
        moved=tmp/"relocated";moved.mkdir()
        import shutil
        shutil.copy2(output,moved/output.name)
        for folder in tmp.glob(output.name+".parts-*"):shutil.copytree(folder,moved/folder.name)
        assert decode(moved/output.name)==leaves,"shard paths must be relative to the entry asset"
        print("CXW canonical voxmesh cook: 8 tests PASS")


if __name__=="__main__":main()
