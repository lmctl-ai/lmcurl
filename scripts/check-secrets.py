#!/usr/bin/env python3
"""Scan distributable files/artifacts without ever printing credential values."""
import argparse,os,re,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--artifacts',action='store_true');args=p.parse_args()
files=set()
for raw in subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=root).split(b'\0'):
 if raw:files.add(root/os.fsdecode(raw))
if args.artifacts:
 for folder in ('data','reports','cassettes','images'):
  files.update(x for x in (root/folder).rglob('*') if x.is_file())
secrets=[v.encode() for n in ('OPENROUTER_API_KEY','DEEPSEEK_API_KEY','OLLAMA_API_KEY','OPENAI_API_KEY','GITHUB_TOKEN','GH_TOKEN') if (v:=os.getenv(n)) and len(v)>=8]
pattern=re.compile(rb'(?<![A-Za-z0-9])(?:sk-[A-Za-z0-9_-]{20,}|ghp_[A-Za-z0-9]{25,}|github_pat_[A-Za-z0-9_]{30,})')
bad=[]
for f in sorted(files):
 if not f.is_file():continue
 data=f.read_bytes()
 if any(s in data for s in secrets) or pattern.search(data):bad.append(str(f.relative_to(root)))
if bad:
 for f in bad:print('credential-like content: '+f,file=sys.stderr)
 raise SystemExit(1)
print('PASS secret scan: '+str(len(files))+' files; no key values or credential-shaped literals')
