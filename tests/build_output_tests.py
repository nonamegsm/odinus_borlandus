"""Manifest-selected build output regression tests; no compiler or device is used."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('bcc_build_output',ROOT/'build.py')
build=importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

class BuildOutputTests(unittest.TestCase):
    def setUp(self):
        self.scratch=tempfile.TemporaryDirectory(prefix='bcc output with spaces ')
        self.root=Path(self.scratch.name)
        self.output=self.root/'bin/Debug'
        self.output.mkdir(parents=True)
        self.exe=self.output/'OdinBCC.native.exe'
        self.exe.write_bytes(b'current native executable')
        (self.output/'OdinBCC.exe').write_bytes(b'old mapped executable')
        self.manifest={'configuration':'Debug','native_engine':True,'private_runtime_dll_required':False,
                       'executable':{'file':self.exe.name,'bytes':self.exe.stat().st_size,'sha256':sha(self.exe)}}
        for group,name in (('sources','src/Main.cpp'),('headers','src/UiLogic.h'),('objects','build/Debug/engine.obj')):
            path=self.root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(group.encode())
            self.manifest[group]=[{'file':name,'sha256':sha(path)}]
        self.save()
        self.pe=patch.object(build,'pe_imports',return_value=[])
        self.pe.start()
    def tearDown(self):self.pe.stop();self.scratch.cleanup()
    def save(self):(self.output/'build-manifest.json').write_text(json.dumps(self.manifest),encoding='utf-8')
    def check(self):return build.verified_output('Debug',self.root)
    def test_fallback_wins_over_stale_canonical(self):self.assertEqual(self.check(),self.exe)
    def test_configuration_and_native_contract(self):
        for key,value in (('configuration','Release'),('native_engine',False),('private_runtime_dll_required',True)):
            original=self.manifest[key];self.manifest[key]=value;self.save()
            with self.assertRaises(RuntimeError):self.check()
            self.manifest[key]=original
    def test_executable_name_cannot_escape_output(self):
        for name in ('','../outside.exe','..\\outside.exe','C:\\outside.exe','/outside.exe','C:outside.exe','OdinBCC.exe:stream','..','.'):
            self.manifest['executable']['file']=name;self.save()
            with self.assertRaises(RuntimeError):self.check()
    def test_stale_or_missing_executable_rejected(self):
        self.exe.write_bytes(b'stale')
        with self.assertRaises(RuntimeError):self.check()
        self.exe.unlink()
        with self.assertRaises(RuntimeError):self.check()
    def test_tampered_source_header_or_object_rejected(self):
        for group in ('sources','headers','objects'):
            path=self.root/self.manifest[group][0]['file'];original=path.read_bytes();path.write_bytes(b'changed')
            with self.assertRaises(RuntimeError):self.check()
            path.write_bytes(original)
    def test_incomplete_snapshot_rejected(self):
        for group in ('sources','headers','objects'):
            original=self.manifest[group];self.manifest[group]=[];self.save()
            with self.assertRaises(RuntimeError):self.check()
            self.manifest[group]=original
    def test_snapshot_path_cannot_escape_project(self):
        self.manifest['headers'][0]['file']='../outside.h';self.save()
        with self.assertRaises(RuntimeError):self.check()

if __name__=='__main__':unittest.main()
