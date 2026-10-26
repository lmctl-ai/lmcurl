#!/usr/bin/env python3
import json,re,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
host=sys.argv[1] if len(sys.argv)>1 else str(root/'build/lmcurl')
types,functions,context={},{},{}
pattern=re.compile(r'^(?:fn (\w+)|method (\w+)\.(\w+))\((.*?)\) -> (\w+)$')
for line in subprocess.check_output([host,'--dsl-signatures'],text=True,timeout=30).splitlines():
 if line.startswith('context '):name,typ=line[8:].split(': ');context[name]=typ;continue
 m=pattern.fullmatch(line)
 if not m:raise SystemExit('invalid registry signature')
 fn,typ,method,params,result=m.groups();entry={'params':params.split(', ') if params else [],'returns':result}
 if fn:functions.setdefault(fn,[]).append(entry)
 else:types.setdefault(typ,{'methods':{}})['methods'].setdefault(method,[]).append(entry)
schema={'dsl.schema':2,'name':'lmcurl','types':dict(sorted(types.items())),'functions':dict(sorted(functions.items())),'context':dict(sorted(context.items()))}
p=subprocess.run([sys.executable,str(root/'scripts/merge-std.py'),'--host-only','-','-'],input=(json.dumps(schema,indent=2)+'\n').encode(),capture_output=True,check=True,timeout=30)
sys.stdout.buffer.write(p.stdout)
