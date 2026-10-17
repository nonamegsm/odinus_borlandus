"""Offline smoke tests for an owned Odin BCC window; never click device operations."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile
import time

U=C.WinDLL('user32', use_last_error=True)
G=C.WinDLL('gdi32', use_last_error=True)
HWND=W.HWND
LPARAM=C.c_ssize_t
WPARAM=C.c_size_t
LRESULT=C.c_ssize_t
U.SendMessageW.argtypes=[HWND,W.UINT,WPARAM,LPARAM]; U.SendMessageW.restype=LRESULT
U.GetDlgItem.argtypes=[HWND,C.c_int]; U.GetDlgItem.restype=HWND
U.GetWindowTextLengthW.argtypes=[HWND]; U.GetWindowTextLengthW.restype=C.c_int
U.GetWindowTextW.argtypes=[HWND,W.LPWSTR,C.c_int]; U.GetWindowTextW.restype=C.c_int
U.SetWindowTextW.argtypes=[HWND,W.LPCWSTR]
U.GetWindowRect.argtypes=[HWND,C.POINTER(W.RECT)]
U.GetClientRect.argtypes=[HWND,C.POINTER(W.RECT)]
U.ScreenToClient.argtypes=[HWND,C.POINTER(W.POINT)]
U.IsWindowEnabled.argtypes=[HWND]; U.IsWindowEnabled.restype=W.BOOL
U.IsWindow.argtypes=[HWND]; U.IsWindow.restype=W.BOOL
U.SetWindowPos.argtypes=[HWND,HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.UINT]
U.PostMessageW.argtypes=[HWND,W.UINT,WPARAM,LPARAM]
U.GetWindowThreadProcessId.argtypes=[HWND,C.POINTER(W.DWORD)]
CALLBACK=C.WINFUNCTYPE(W.BOOL,HWND,LPARAM)
U.EnumWindows.argtypes=[CALLBACK,LPARAM]
U.GetClassNameW.argtypes=[HWND,W.LPWSTR,C.c_int]
U.GetDC.argtypes=[HWND]; U.GetDC.restype=W.HDC
U.ReleaseDC.argtypes=[HWND,W.HDC]
G.CreateCompatibleDC.argtypes=[W.HDC]; G.CreateCompatibleDC.restype=W.HDC
G.CreateCompatibleBitmap.argtypes=[W.HDC,C.c_int,C.c_int]; G.CreateCompatibleBitmap.restype=W.HBITMAP
G.SelectObject.argtypes=[W.HDC,W.HGDIOBJ]; G.SelectObject.restype=W.HGDIOBJ
G.DeleteObject.argtypes=[W.HGDIOBJ]
G.DeleteDC.argtypes=[W.HDC]
G.GetDIBits.argtypes=[W.HDC,W.HBITMAP,W.UINT,W.UINT,C.c_void_p,C.c_void_p,W.UINT]
U.PrintWindow.argtypes=[HWND,W.HDC,W.UINT]; U.PrintWindow.restype=W.BOOL
U.ShowWindow.argtypes=[HWND,C.c_int]
U.UpdateWindow.argtypes=[HWND]
U.RedrawWindow.argtypes=[HWND,C.c_void_p,W.HRGN,W.UINT]


def wait_for(predicate, timeout=15):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        result=predicate()
        if result:
            return result
        time.sleep(.05)
    raise AssertionError('Timed out waiting for an owned UI state')


def text(hwnd):
    buf=C.create_unicode_buffer(U.SendMessageW(hwnd,0xE,0,0)+1)
    U.SendMessageW(hwnd,0xD,len(buf),C.cast(buf,C.c_void_p).value)
    return buf.value


def set_text(hwnd,value):
    buffer=C.create_unicode_buffer(value)
    assert U.SendMessageW(hwnd,0xC,0,C.cast(buffer,C.c_void_p).value)
    assert text(hwnd)==value, ('Edit text readback mismatch',value,text(hwnd))


def find_window(pid):
    found=[]
    @CALLBACK
    def visit(hwnd,_):
        actual=W.DWORD()
        U.GetWindowThreadProcessId(hwnd,C.byref(actual))
        cls=C.create_unicode_buffer(256)
        U.GetClassNameW(hwnd,cls,len(cls))
        if actual.value==pid and cls.value=='OdinBCCMainWindow':
            found.append(hwnd)
        return True
    U.EnumWindows(visit,0)
    return found[0] if found else None


def rect(hwnd,parent=None):
    r=W.RECT(); assert U.GetWindowRect(hwnd,C.byref(r))
    if parent:
        a,b=W.POINT(r.left,r.top),W.POINT(r.right,r.bottom)
        U.ScreenToClient(parent,C.byref(a)); U.ScreenToClient(parent,C.byref(b))
        return [a.x,a.y,b.x,b.y]
    return [r.left,r.top,r.right,r.bottom]


def screenshot(hwnd,destination):
    U.ShowWindow(hwnd,4)
    U.UpdateWindow(hwnd)
    U.RedrawWindow(hwnd,None,None,0x185)
    time.sleep(.2)
    r=rect(hwnd); width,height=r[2]-r[0],r[3]-r[1]
    screen=U.GetDC(None); dc=G.CreateCompatibleDC(screen)
    bitmap=G.CreateCompatibleBitmap(screen,width,height); previous=G.SelectObject(dc,bitmap)
    try:
        assert U.PrintWindow(hwnd,dc,0), 'PrintWindow failed'
        G.SelectObject(dc,previous)
        stride=((width*24+31)//32)*4
        info=struct.pack('<IiiHHIIiiII',40,width,height,1,24,0,stride*height,0,0,0,0)
        header=C.create_string_buffer(info)
        pixels=C.create_string_buffer(stride*height)
        assert G.GetDIBits(dc,bitmap,0,height,pixels,header,0)==height
        assert len(set(pixels.raw))>4, 'Blank capture is not visual verification'
        data=struct.pack('<2sIHHI',b'BM',54+len(pixels),0,0,54)+info+pixels.raw
        destination.write_bytes(data)
        try:
            from PIL import Image
            with Image.open(io.BytesIO(data)) as image:
                image.save(destination.with_suffix('.png'))
        except ImportError:
            pass
    finally:
        G.DeleteObject(bitmap); G.DeleteDC(dc); U.ReleaseDC(None,screen)
        U.ShowWindow(hwnd,0)


class OwnedUI:
    def __init__(self,exe,cwd):
        startup=subprocess.STARTUPINFO()
        startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow=0
        self.process=subprocess.Popen([str(exe)],cwd=cwd,startupinfo=startup)
        self.window=wait_for(lambda:find_window(self.process.pid))
    def control(self,identifier):
        result=U.GetDlgItem(self.window,identifier)
        assert result, f'Missing control {identifier}'
        return result
    def log(self): return text(self.control(164))
    def click_offline(self,identifier):
        assert identifier in (203,206), 'Only offline plan and cancel are permitted'
        assert U.IsWindowEnabled(self.control(identifier)), f'Control {identifier} is disabled'
        U.SendMessageW(self.control(identifier),0xF5,0,0)
    def close(self):
        if self.process.poll() is None:
            U.PostMessageW(self.window,0x10,0,0)
            try: self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.terminate(); self.process.wait(timeout=5)
                raise AssertionError('Owned offline test window failed to close cleanly')
        assert self.process.returncode==0


def layout_case(ui,width,height,artifacts):
    assert U.SetWindowPos(ui.window,None,0,0,width,height,0x14)
    time.sleep(.15)
    bounds=rect(ui.window)
    assert bounds[2]-bounds[0]==width and bounds[3]-bounds[1]==height
    client=W.RECT(); U.GetClientRect(ui.window,C.byref(client))
    ids=list(range(120,126))+list(range(201,208))+[103,141,142,160,161,162,163,164,165]+list(range(300,306))+list(range(310,316))
    controls={}
    for identifier in ids:
        controls[str(identifier)]=r=rect(ui.control(identifier),ui.window)
        assert 0<=r[0]<r[2]<=client.right and 0<=r[1]<r[3]<=client.bottom, (identifier,r,[client.right,client.bottom])
    for index in range(6):
        path=controls[str(120+index)]; browse=controls[str(300+index)]; clear=controls[str(310+index)]
        assert path[2]<=browse[0] and browse[2]<=clear[0]
    log=controls['164']; status=controls['165']
    assert log[3]<=status[1] and log[3]-log[1]>=60
    screenshot(ui.window,artifacts/f'ui-{width}x{height}.bmp')
    return {'window':[width,height],'client':[client.right,client.bottom],'controls':controls}


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('exe',type=Path)
    parser.add_argument('--artifacts',type=Path,default=Path(__file__).parent/'ui-artifacts')
    parser.add_argument('--firmware-dir',type=Path,help='Optional actual firmware offline plan; never selects a USB device')
    args=parser.parse_args(); exe=args.exe.resolve(); artifacts=args.artifacts.resolve()
    artifacts.mkdir(parents=True,exist_ok=True)
    results={'hardwareActions':0,'deviceEnumeration':False,'cases':[]}
    with tempfile.TemporaryDirectory(prefix='odin-bcc-ui-') as scratch_name:
        scratch=Path(scratch_name)
        missing=scratch/'standalone-exe'; missing.mkdir(); shutil.copy2(exe,missing/exe.name)
        ui=OwnedUI(missing/exe.name,missing)
        try:
            wait_for(lambda:'Native Borland engine linked directly' in ui.log())
            assert all(U.IsWindowEnabled(ui.control(n)) for n in (201,202,203,204,205,207))
            assert not U.IsWindowEnabled(ui.control(206))
            assert not list(missing.glob('*.dll'))
            assert U.SendMessageW(ui.control(103),0x146,0,0)==0
            results['cases'].append('copied EXE starts standalone with native engine and no private DLL')
        finally: ui.close()
        ui=OwnedUI(exe,exe.parent)
        try:
            wait_for(lambda:'Native Borland engine linked directly' in ui.log())
            assert U.SendMessageW(ui.control(103),0x146,0,0)==0
            assert U.SendMessageW(ui.control(103),0x147,0,0)&0xFFFFFFFF==0xFFFFFFFF
            assert not U.IsWindowEnabled(ui.control(206))
            assert U.SendMessageW(ui.control(161),0x408,0,0)==0
            results['layouts']=[layout_case(ui,1140,840,artifacts),layout_case(ui,1040,760,artifacts)]
            ui.click_offline(203)
            wait_for(lambda:'Choose at least one firmware image archive.' in ui.log())
            assert U.IsWindowEnabled(ui.control(203))
            results['cases'].append('blank archive selection rejected before worker')
            set_text(ui.control(121),str(scratch/'absent.tar'))
            ui.click_offline(203)
            wait_for(lambda:'FAILED:' in ui.log() and U.IsWindowEnabled(ui.control(203)))
            assert not U.IsWindowEnabled(ui.control(206))
            assert 'any write: yes' not in ui.log()
            results['cases'].append('nonexistent firmware file returns failure and restores controls')
            # A private plain TAR is hashed/decoded only; there is no device path.
            archive=scratch/'cancel-fixture.tar'
            with tarfile.open(archive,'w',format=tarfile.USTAR_FORMAT) as tar:
                member=tarfile.TarInfo('boot.img'); member.size=32*1024*1024
                tar.addfile(member,io.BytesIO(b'\0'*member.size))
            set_text(ui.control(121),str(archive))
            before=ui.log().count('FAILED:')
            ui.click_offline(203); ui.click_offline(206)
            wait_for(lambda:ui.log().count('FAILED:')>before and U.IsWindowEnabled(ui.control(203)))
            assert 'Cancellation requested.' in ui.log()
            assert 'cancel' in text(ui.control(165)).lower()
            assert not U.IsWindowEnabled(ui.control(206))
            assert 'Refresh devices ---' not in ui.log()
            results['cases'].append('offline verification cancellation drains and restores controls')
            if args.firmware_dir:
                def firmware(prefix):
                    matches=list(args.firmware_dir.glob(prefix+'*.tar.md5'))
                    assert len(matches)==1, (prefix,matches)
                    return str(matches[0].resolve())
                chosen=[firmware('BL_'),firmware('AP_'),firmware('CP_'),'',firmware('HOME_CSC_'),firmware('HOME_CSC_')]
                for index,value in enumerate(chosen):
                    set_text(ui.control(120+index),value)
                U.SendMessageW(ui.control(141),0x14E,0,0)
                previous=ui.log().count('SUCCESS:')
                ui.click_offline(203)
                wait_for(lambda:ui.log().count('SUCCESS:')>previous and U.IsWindowEnabled(ui.control(203)),timeout=240)
                log=ui.log()
                assert 'images 0/26' in log and 'any write: no' in log
                assert 'finalized: yes' not in log
                assert 'Using matching CSC for PIT metadata only:' in log
                assert 'Archive role 6: '+chosen[4] in log
                assert 'Archive role 7: '+firmware('CSC_') in log
                assert 'Archive role 4:' not in log
                assert U.SendMessageW(ui.control(103),0x146,0,0)==0
                assert U.SendMessageW(ui.control(161),0x408,0,0)==1000
                assert 'Bytes acknowledged: 0/11882352976' in log
                for expected in ('6413ceffbfedc011aa4194cf556904cb62bf85adfccf031ffdf227a1fd53b861',
                                 'df0834a2a6ada27aac387ee29b73654f5aef393cb52d70bf3263341450965eb0',
                                 '4d24a18f95bfc442cb2988e130df08b805309f72ac6cfcbcc12b21f15082a306'):
                    assert 'SHA256 '+expected in log, expected
                results['knownImageSha256Matches']=3
                results['realPlanTotalBytes']=11882352976
                results['realPlanImages']=26
                results['cases'].append('actual BL/AP/CP/HOME in USERDATA and reference yields 26-image offline plan; only reference becomes CSC')
                screenshot(ui.window,artifacts/'ui-real-plan.bmp')

            (artifacts/'ui-session.log').write_text(ui.log(),encoding='utf-8')
        finally: ui.close()
    results['success']=True
    (artifacts/'ui-smoke-results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    print('PASS owned UI smoke: '+str(len(results['cases']))+' offline cases, two layouts, no device enumeration or USB I/O.')


if __name__=='__main__':
    main()
