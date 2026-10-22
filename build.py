#!/usr/bin/env python3
"""Compile GUI, complete Odin engine and LZ4 into one native classic BCC32 executable."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
ROOT = Path(__file__).resolve().parent
DEFAULT_BDS = Path(r"C:\Program Files (x86)\Embarcadero\Studio\15.0")

def digest(path):
    h=hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda:stream.read(1024*1024),b""):
            h.update(data)
    return h.hexdigest()

def quote(value):
    value=str(value)
    if any(c in value for c in ('"','\n','\r')):
        raise ValueError("Invalid compiler path: "+value)
    return '"'+value+'"'

def pe_imports(path):
    data=path.read_bytes()
    pe=struct.unpack_from("<I",data,60)[0]
    count=struct.unpack_from("<H",data,pe+6)[0]
    optional_size=struct.unpack_from("<H",data,pe+20)[0]
    optional=pe+24
    if struct.unpack_from("<H",data,optional)[0]!=0x10b:raise RuntimeError("Expected PE32 optional header")
    import_rva=struct.unpack_from("<I",data,optional+104)[0]
    sections=optional+optional_size
    def offset(rva):
        for index in range(count):
            section=sections+index*40
            virtual_size,address,raw_size,raw=struct.unpack_from("<IIII",data,section+8)
            if address<=rva<address+max(virtual_size,raw_size):
                result=raw+rva-address
                if result>=len(data):raise RuntimeError("PE import RVA outside image file")
                return result
        raise RuntimeError("PE import RVA has no section")
    names=[]
    if import_rva:
        descriptor=offset(import_rva)
        for index in range(1024):
            entry=struct.unpack_from("<IIIII",data,descriptor+index*20)
            if not any(entry):break
            start=offset(entry[3]);end=data.find(b"\0",start,start+512)
            if end<0:raise RuntimeError("Unterminated PE import name")
            names.append(data[start:end].decode("ascii"))
        else:raise RuntimeError("Too many PE import descriptors")
    if any(name.lower()=="easyjtagodin4core.dll" for name in names):
        raise RuntimeError("Native executable unexpectedly imports the private Odin DLL")
    return sorted(set(names),key=str.lower)



def verified_output(configuration, root=ROOT):
    """Return the manifest-named native executable only for an intact build snapshot."""
    root=Path(root).resolve()
    output=root/"bin"/configuration
    manifest=json.loads((output/"build-manifest.json").read_text(encoding="utf-8"))
    if manifest.get("configuration")!=configuration or manifest.get("native_engine") is not True or manifest.get("private_runtime_dll_required") is not False:
        raise RuntimeError("Build manifest is not the requested native configuration")
    record=manifest.get("executable",{})
    name=record.get("file","")
    if not name or "/" in name or "\\" in name or ":" in name or name in (".",".."):
        raise RuntimeError("Unsafe executable name in build manifest")
    executable=output/name
    try:executable.resolve().relative_to(output.resolve())
    except ValueError:raise RuntimeError("Executable resolves outside configured output")
    if not executable.is_file() or executable.stat().st_size!=record.get("bytes") or digest(executable)!=record.get("sha256"):
        raise RuntimeError("Built executable does not match its manifest")
    for group in ("sources","headers","objects"):
        records=manifest.get(group)
        if not records:
            raise RuntimeError("Build manifest omits "+group)
        for item in records:
            path=(root/item["file"]).resolve()
            try:path.relative_to(root)
            except ValueError:raise RuntimeError("Build snapshot path escapes project: "+item["file"])
            if not path.is_file() or digest(path)!=item["sha256"]:
                raise RuntimeError("Build snapshot changed; rebuild before testing: "+item["file"])
    pe_imports(executable)
    return executable


def verify_vendor():
    vendor=ROOT/"engine/vendor"
    for manifest_name,subdirectory in (("LZ4.json","lz4"),("UPSTREAM.json","upstream")):
        metadata=json.loads((vendor/manifest_name).read_text(encoding="utf-8"))
        if metadata.get("commit")!="b8630fb39624c76cdce0e196c57f7285d63286d3":
            raise RuntimeError("Unexpected pinned vendor revision: "+manifest_name)
        for relative,record in metadata["files"].items():
            path=(vendor/subdirectory/relative).resolve()
            path.relative_to((vendor/subdirectory).resolve())
            if not path.is_file() or digest(path)!=record["sha256"]:
                raise RuntimeError("Pinned vendor source changed: "+relative)
        notice=metadata.get("redistribution_notice")
        if notice and digest(vendor/subdirectory/"LICENSE.txt")!=notice["sha256"]:
            raise RuntimeError("Pinned LZ4 redistribution notice changed")

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration",choices=("Debug","Release"),default="Debug")
    parser.add_argument("--bds",type=Path)
    parser.add_argument("--clean",action="store_true")
    parser.add_argument("--objects-only",action="store_true",help="Compile GUI/engine/LZ4 OMF objects without linking")
    args=parser.parse_args()
    bds=(args.bds or Path(os.environ.get("BDS",str(DEFAULT_BDS)))).resolve()
    out=ROOT/"bin"/args.configuration
    intermediate=ROOT/"build"/args.configuration
    if args.clean:
        for directory in (out,intermediate):
            if directory.resolve().parent not in ((ROOT/"bin").resolve(),(ROOT/"build").resolve()):
                raise RuntimeError("Refusing to clean outside project output directories")
            if directory.exists():shutil.rmtree(directory)
        print("Cleaned "+args.configuration)
        return 0
    verify_vendor()
    compiler=bds/"bin"/"bcc32.exe"
    if not compiler.is_file():raise RuntimeError("bcc32.exe not found; pass --bds or set BDS: "+str(compiler))
    out.mkdir(parents=True,exist_ok=True)
    intermediate.mkdir(parents=True,exist_ok=True)
    includes=[ROOT/"include",ROOT/"engine/include",ROOT/"engine/src",ROOT/"engine/vendor/lz4",
              bds/"include",bds/"include/windows/crtl",bds/"include/dinkumware",bds/"include/windows/rtl",bds/"include/windows/sdk"]
    libraries=[bds/"lib/win32/release",bds/"lib/win32/release/psdk"]
    common=["-tW","-tWM","-H-","-DWIN32","-D_WIN32_WINNT=0x0601","-DUNICODE","-D_UNICODE","-DEJ_ODIN4_CORE_STATIC",
            "-I"+";".join(quote(p) for p in includes),"-L"+";".join(quote(p) for p in libraries)]
    common += ["-v","-y","-Od","-D_DEBUG"] if args.configuration=="Debug" else ["-O2","-v-","-DNDEBUG"]
    env=os.environ.copy()
    env["PATH"]=str(bds/"bin")+os.pathsep+env.get("PATH","")
    # Includes the native firmware identity verifier in both IDE and CLI builds.
    if not (ROOT/"engine/src/Odin4FirmwareIdentity.cpp").is_file():
        raise RuntimeError("Missing native firmware identity verifier")
    engine_sources=sorted((ROOT/"engine/src").glob("*.cpp"))
    if not engine_sources:raise RuntimeError("No engine sources found")
    sources=[ROOT/"src/Main.cpp"]+engine_sources+[ROOT/"engine/vendor/lz4"/(name+".c") for name in ("lz4","lz4hc","lz4frame","xxhash")]
    objects=[]
    print("Compiler: "+str(compiler),flush=True)
    for source in sources:
        if not source.is_file():raise RuntimeError("Missing source: "+str(source))
        group="lz4" if source.suffix==".c" else "gui" if source.parent.name=="src" and source.parent.parent==ROOT else "engine"
        directory=intermediate/group
        directory.mkdir(parents=True,exist_ok=True)
        obj=directory/(source.stem+".obj")
        options=common+["-c","-n"+quote(directory)]
        if source.suffix==".c":options += ["-P"]
        options += [quote(source)]
        response=directory/(source.stem+".rsp")
        response.write_text("\n".join(options)+"\n",encoding="mbcs")
        result=subprocess.run([str(compiler),"@"+str(response)],cwd=directory,env=env)
        if result.returncode:return result.returncode
        if not obj.is_file():raise RuntimeError("Compiler produced no object: "+str(obj))
        objects.append(obj)
    (intermediate/"objects.json").write_text(json.dumps([str(p) for p in objects],indent=2)+"\n",encoding="utf-8")
    if args.objects_only:
        print("Compiled "+str(len(objects))+" native BCC32 objects")
        return 0
    pending=out/"OdinBCC.pending.exe"
    executable=out/"OdinBCC.exe"
    if pending.exists():pending.unlink()
    link=common+["-e"+quote(os.path.relpath(pending,intermediate))]+[quote(p) for p in objects]
    link += ["comctl32.lib","comdlg32.lib","shell32.lib","setupapi.lib","advapi32.lib","ole32.lib","ws2_32.lib"]
    response=intermediate/"bcc32-link.rsp"
    response.write_text("\n".join(link)+"\n",encoding="mbcs")
    result=subprocess.run([str(compiler),"@"+str(response)],cwd=intermediate,env=env)
    if result.returncode:return result.returncode
    with pending.open("rb") as stream:
        header=stream.read(64)
        if header[:2]!=b"MZ":raise RuntimeError("Linker output is not a Windows executable")
        stream.seek(struct.unpack_from("<I",header,60)[0]);pe=stream.read(6)
        if pe[:4]!=b"PE\0\0" or struct.unpack_from("<H",pe,4)[0]!=0x14c:raise RuntimeError("Linker output is not x86 PE")
    imports=pe_imports(pending)
    try:
        os.replace(pending,executable)
    except PermissionError:
        executable=out/"OdinBCC.native.exe"
        os.replace(pending,executable)
        print("Existing OdinBCC.exe is in use; built "+str(executable),flush=True)
    notices=[]
    for name in ("LICENSE","NOTICE"):
        source=ROOT/name
        if source.is_file():
            target=out/name
            shutil.copy2(source,target)
            if digest(source)!=digest(target):raise RuntimeError("Notice copy integrity failure: "+name)
            notices.append({"file":name,"sha256":digest(target)})
    manifest={"configuration":args.configuration,"compiler":str(compiler),"native_engine":True,"private_runtime_dll_required":False,
              "executable":{"file":executable.name,"sha256":digest(executable),"bytes":executable.stat().st_size},
              "objects":[{"file":str(p.relative_to(ROOT)),"sha256":digest(p)} for p in objects],
              "sources":[{"file":str(p.relative_to(ROOT)),"sha256":digest(p)} for p in sources],
              "headers":[{"file":str(p.relative_to(ROOT)),"sha256":digest(p)} for directory in (ROOT/"include",ROOT/"engine/include",ROOT/"engine/src",ROOT/"src") for p in sorted(directory.glob("*.h"))],
              "pe_imports":imports,"notices":notices}
    (out/"build-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    print("Built standalone native engine: "+str(executable)+"; "+str(len(objects))+" BCC32 objects; no private Odin DLL")
    return 0

if __name__=="__main__":
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError) as error:
        print("Build failed: "+str(error),file=sys.stderr);sys.exit(1)
