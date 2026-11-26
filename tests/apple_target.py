#!/usr/bin/env python3
"""Synthetic Mach-O bounds/target tests; this is NOT a macOS execution test."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cxx',required=True);p.add_argument('--cmake',default='cmake');a=p.parse_args();count=0
    with tempfile.TemporaryDirectory(prefix='bolt-macos-target-') as temp:
        root=Path(temp);exe=root/('check.exe' if os.name=='nt' else 'check')
        msvc=Path(a.cxx).name.lower() in ('cl','cl.exe')
        flags=['/nologo','/std:c++20','/EHsc','/utf-8','/I'+str(ROOT/'include'),str(ROOT/'tests/apple_target.cpp'),'/Fe:'+str(exe)] if msvc else ['-std=c++20','-Wall','-Wextra','-pedantic','-I'+str(ROOT/'include'),str(ROOT/'tests/apple_target.cpp'),'-o',str(exe)]
        subprocess.run([a.cxx,*flags],check=True,cwd=root)
        def check(args,output=None,error=None):
            nonlocal count
            r=subprocess.run([str(exe),*map(str,args)],text=True,capture_output=True)
            assert r.returncode==(2 if error is not None else 0),(args,r.returncode,r.stdout,r.stderr)
            if output is not None:assert r.stdout==output,(args,r.stdout,output)
            if error is not None:assert error in r.stderr,(args,r.stderr,error)
            count+=1
        def version(major,minor=0,patch=0):return major<<16|minor<<8|patch
        arm=0x0100000c;x86=0x01000007
        def thin(cpu=arm,minimum=version(26),endian='<',legacy=False):
            command=struct.pack(endian+'IIII',0x24,16,minimum,version(26)) if legacy else struct.pack(endian+'IIIIII',0x32,24,1,minimum,version(26),0)
            return struct.pack(endian+'IIIIIIII',0xfeedfacf,cpu,0,6,1,len(command),0,0)+command
        image=root/'runtime.dylib';image.write_bytes(thin())
        check(['read',image,'arm64','26.0'],'26.0.0')
        check(['read',image,'arm64','27.0'],'26.0.0')
        check(['read',image,'arm64','11.0'],error='requires macOS 26.0.0')
        check(['read',image,'x86_64'],error='CPU architecture')
        for endian in ['<','>']:
            for legacy in [False,True]:
                image.write_bytes(thin(x86,version(10,15,7),endian,legacy));check(['read',image,'x86_64'],'10.15.7')
        for wide in [False,True]:
            for endian in ['>','<']:
                first=thin(x86,version(12));second=thin(arm,version(15,5))
                header=struct.pack(endian+'II',0xcafebabf if wide else 0xcafebabe,2);stride=32 if wide else 20;at=8+2*stride
                def entry(cpu,offset,data):return struct.pack(endian+('IIQQII' if wide else 'IIIII'),cpu,0,offset,len(data),*([0,0] if wide else [0]))
                image.write_bytes(header+entry(x86,at,first)+entry(arm,at+len(first),second)+first+second)
                check(['read',image,'arm64'],'15.5.0');check(['read',image,'x86_64'],'12.0.0')
        for damaged in [b'',b'not mach-o',thin()[:31],thin()[:-1],thin()[:36]+struct.pack('<I',0xffffffff)+thin()[40:],thin()[:20]+struct.pack('<I',0xfffffff0)+thin()[24:]]:
            image.write_bytes(damaged);check(['read',image,'arm64'],error='')

        check(['version','11.0'],'11.0.0');check(['version','26.1.3'],'26.1.3')
        for bad in ['','26','26.0.','26.0.0.0','-1.0','26.256','99999.0','hello']:check(['version',bad],error='')
        sdk=root/'SDK';sdk.mkdir();check(['sdk',sdk,'15.0'],'15.0')
        (sdk/'SDKSettings.json').write_text('{"Version":"26.1"}')
        check(['sdk',sdk,'15.0'],'26.1')
        (sdk/'SDKSettings.json').write_text('{"Version":"garbage"}')
        check(['sdk',sdk,'15.0'],'15.0')
        cmake_script=root/'target.cmake';cmake_script.write_text('include("'+(ROOT/'cmake/BoltAppleTarget.cmake').as_posix()+'")\nmessage(STATUS "minimum=${CMAKE_OSX_DEPLOYMENT_TARGET}")\n')
        def configure(options,expected=None,error=None,environment=None):
            nonlocal count
            env=os.environ.copy();env.pop('MACOSX_DEPLOYMENT_TARGET',None)
            if environment:env.update(environment)
            result=subprocess.run([a.cmake,*options,'-P',str(cmake_script)],env=env,capture_output=True,text=True)
            text=result.stdout+result.stderr
            assert (result.returncode==0)==(error is None),(options,text)
            assert (error if error is not None else expected) in text,(options,text)
            count+=1
        configure(['-DCMAKE_SYSTEM_NAME=Darwin','-DCMAKE_OSX_DEPLOYMENT_TARGET=13.2'],expected='minimum=13.2')
        configure(['-DCMAKE_SYSTEM_NAME=Darwin'],expected='minimum=15.0',environment={'MACOSX_DEPLOYMENT_TARGET':'15.0'})
        configure(['-DCMAKE_SYSTEM_NAME=Darwin','-DCMAKE_OSX_DEPLOYMENT_TARGET=13.2'],expected='minimum=13.2',environment={'MACOSX_DEPLOYMENT_TARGET':'15.0'})
        configure(['-DCMAKE_SYSTEM_NAME=Darwin','-DCMAKE_HOST_APPLE=FALSE'],error='Cross-building for macOS requires')
        configure(['-DCMAKE_SYSTEM_NAME=Darwin','-DCMAKE_OSX_DEPLOYMENT_TARGET=bad'],error='numeric macOS version')
        configure(['-DCMAKE_SYSTEM_NAME=Linux','-DCMAKE_HOST_APPLE=FALSE'],expected='minimum=')
    print(f'{count} Apple target format, bounds, SDK and CMake selection checks passed')
if __name__=='__main__':main()
