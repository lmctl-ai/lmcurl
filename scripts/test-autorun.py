#!/usr/bin/env python3
"""Verify every public example from an empty directory with a fresh shared tool cache."""
import hashlib,json,os,shutil,subprocess,tempfile,time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
env=dict(os.environ,LMCURL_LIVE='0')
if not env.get('GITHUB_TOKEN'):
 env['GITHUB_TOKEN']=subprocess.check_output(['gh','auth','token'],text=True,timeout=30).strip()
cli=shutil.which('lmctl');assert cli,'lmctl is required'
# Existing PATH installations would hide whether the published host installs correctly.
env['PATH']=os.pathsep.join(p for p in env['PATH'].split(os.pathsep) if p and not any((Path(p)/name).is_file() for name in ('lmcurl','lmplot','lmdraw')))
assert all(shutil.which(name,path=env['PATH']) is None for name in ('lmcurl','lmplot','lmdraw'))
rows=[]
with tempfile.TemporaryDirectory(prefix='lmcurl-autorun-') as td:
 base=Path(td);cache=base/'tool-cache';env['LMSCRIPT_TOOL_CACHE']=str(cache);env['LMCTL_HOME']=str(base/'lmctl')
 for row in json.loads((root/'examples/catalog.json').read_text()):
  work=base/row['name'];work.mkdir();url='https://raw.githubusercontent.com/lmctl-ai/lmcurl/main/'+row['script'];start=time.monotonic()
  result=subprocess.run([cli,'autorun',url],cwd=work,env=env,capture_output=True,text=True,timeout=300)
  # Never echo process output: the public result records only status and image metadata.
  assert result.returncode==0,'autorun failed: '+row['name']
  image=work/'images'/(row['name']+'.png');data=image.read_bytes();assert data[:8]==b'\x89PNG\r\n\x1a\n' and len(data)<=307200
  rows.append({'example':row['name'],'url':url,'passed':True,'elapsed_seconds':round(time.monotonic()-start,3),'image_bytes':len(data),'image_sha256':hashlib.sha256(data).hexdigest()})
  print('PASS autorun '+row['name'],flush=True)
 hosts=[p for p in cache.rglob('lmcurl') if p.is_file() and os.access(p,os.X_OK)]
 assert hosts,'autorun did not install lmcurl into the fresh cache'
 host=hosts[0];version=subprocess.check_output([str(host),'--version'],env=env,text=True,timeout=10).strip();assert version=='lmcurl 0.1.0'
 release=hashlib.sha256(host.read_bytes()).hexdigest()
 proof={'version':version,'fresh_tool_cache':True,'hosts_removed_from_path':['lmcurl','lmplot','lmdraw'],'empty_working_directory_per_example':True,'installed_binary_sha256':release,'examples':rows}
 (root/'evidence/autorun-v010.json').write_text(json.dumps(proof,indent=2)+'\n')
print('PASS all '+str(len(rows))+' public autoruns; release binary installed from a fresh cache')
