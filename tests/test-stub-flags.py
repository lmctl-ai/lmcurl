#!/usr/bin/env python3
"""Compile the actual unsupported-platform entry point; never substitute a parser."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='curl stub flags ') as directory:
    work = Path(directory)
    for name, source, symbol in [('schema','schema/lmcurl.dsl.json','embedded_schema'),
                                 ('signatures','schema/lmcurl.signatures.txt','embedded_signatures'),
                                 ('notices','THIRD_PARTY_NOTICES','embedded_notices')]:
        data = (ROOT/source).read_bytes()
        (work/(name+'_embedded.hpp')).write_text('#include <cstddef>\nnamespace lmcurl { inline constexpr unsigned char '+symbol+'[]={'+','.join(map(str,data))+',0}; inline constexpr std::size_t '+symbol+'_size=sizeof('+symbol+')-1;}\n')
    binary = work / ('stub.exe' if os.name == 'nt' else 'stub')
    subprocess.run([*shlex.split(os.environ.get('CXX','c++')),'-std=c++17','-I',str(work),
                    str(ROOT/'cpp/stub.cpp'),'-o',str(binary)],check=True,capture_output=True)
    old = '--'+'dsl'
    failures=[]
    for flag, rest in [(old,['missing.dslb']),(old+'-schema',['--json']),(old+'-signatures',[])]:
        r=subprocess.run([str(binary),flag,*rest],capture_output=True)
        if r.returncode != 2:failures.append((flag,r.returncode))
    assert not failures,('stub retained old options',failures)
    for args,expected in [(['--lmcode-schema','--json'],(ROOT/'schema/lmcurl.dsl.json').read_bytes()),
                          (['--lmcode-signatures'],(ROOT/'schema/lmcurl.signatures.txt').read_bytes())]:
        r=subprocess.run([str(binary),*args],capture_output=True)
        assert (r.returncode,r.stdout,r.stderr)==(0,expected,b''),r
    r=subprocess.run([str(binary),'--lmcode','missing.dslb'],capture_output=True)
    assert r.returncode==3 and b'not supported' in r.stderr and not r.stdout,r
print('PASS actual compiled stub: exact metadata, retired refusal and unchanged unsupported execution')
