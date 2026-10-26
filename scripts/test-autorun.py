#!/usr/bin/env python3
"""Verify every public example from an empty directory with a fresh shared tool cache."""
import argparse,hashlib,json,os,re,shutil,subprocess,tempfile,time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--only-new',action='store_true');parser.add_argument('--version',default=re.search(r'project\(lmcurl VERSION ([0-9.]+)',(root/'CMakeLists.txt').read_text()).group(1));args=parser.parse_args()
env=dict(os.environ,LMCURL_LIVE='0')
if not env.get('GITHUB_TOKEN'):
 env['GITHUB_TOKEN']=subprocess.check_output(['gh','auth','token'],text=True,timeout=30).strip()
cli=shutil.which('lmctl');assert cli,'lmctl is required'
# Existing PATH installations would hide whether the published host installs correctly.
env['PATH']=os.pathsep.join(p for p in env['PATH'].split(os.pathsep) if p and not any((Path(p)/name).is_file() for name in ('lmcurl','lmhtml','lmplot','lmdraw')))
assert all(shutil.which(name,path=env['PATH']) is None for name in ('lmcurl','lmhtml','lmplot','lmdraw'))
rows=[]
with tempfile.TemporaryDirectory(prefix='lmcurl-autorun-') as td:
 base=Path(td);cache=base/'tool-cache';env['LMSCRIPT_TOOL_CACHE']=str(cache);env['LMCTL_HOME']=str(base/'lmctl')
 for row in json.loads((root/'examples/catalog.json').read_text()):
  if args.only_new and not row['name'].startswith('web_'):continue
  work=base/row['name'];work.mkdir();url='https://raw.githubusercontent.com/lmctl-ai/lmcurl/main/'+row['script'];start=time.monotonic()
  result=subprocess.run([cli,'autorun',url],cwd=work,env=env,capture_output=True,text=True,timeout=300)
  # Never echo process output: the public result records only status and image metadata.
  assert result.returncode==0,'autorun failed: '+row['name']
  image=work/'images'/(row['name']+'.png');data=image.read_bytes();assert data[:8]==b'\x89PNG\r\n\x1a\n' and len(data)<=307200
  cost={}
  if row['name'].startswith('web_'):
   ledger=json.loads((work/'reports'/(row['name']+'-cost.json')).read_text())
   events=[json.loads(line) for line in (work/'data/web-requests.jsonl').read_text().splitlines() if line]
   assert ledger['estimated_cost_usd']==0 and len(events)==ledger['requests'] and events
   assert all(v['route'].startswith('/site/') and v['estimated_cost_usd']==0 for v in events)
   if row['name']=='web_robots_crawler':assert all(not v['route'].startswith('/site/private/') for v in events)
   cost={'estimated_cost_usd':0,'requests':ledger['requests']}
  rows.append({'cost':cost,'example':row['name'],'url':url,'passed':True,'elapsed_seconds':round(time.monotonic()-start,3),'image_bytes':len(data),'image_sha256':hashlib.sha256(data).hexdigest()})
  print('PASS autorun '+row['name'],flush=True)
 hosts=[p for p in cache.rglob('lmcurl') if p.is_file() and os.access(p,os.X_OK)]
 assert hosts,'autorun did not install lmcurl into the fresh cache'
 host=hosts[0];version=subprocess.check_output([str(host),'--version'],env=env,text=True,timeout=10).strip();assert version=='lmcurl '+args.version
 release=hashlib.sha256(host.read_bytes()).hexdigest()
 installed_versions={}
 for name in ('lmcurl','lmhtml','lmplot','lmdraw'):
  tools=[p for p in cache.rglob(name) if p.is_file() and os.access(p,os.X_OK)]
  if tools:installed_versions[name]=subprocess.check_output([str(tools[0]),'--version'],env=env,text=True,timeout=10).strip()
 proof={'installed_versions':installed_versions,'version':version,'scope':'new_downloader_parser' if args.only_new else 'all','fresh_tool_cache':True,'hosts_removed_from_path':['lmcurl','lmhtml','lmplot','lmdraw'],'empty_working_directory_per_example':True,'installed_binary_sha256':release,'examples':rows}
 (root/('evidence/autorun-v'+args.version.replace('.','')+'.json')).write_text(json.dumps(proof,indent=2)+'\n')
print('PASS all '+str(len(rows))+' public autoruns; release binary installed from a fresh cache')
