#!/usr/bin/env python3
import argparse,json,os,subprocess,tempfile,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--host',required=True);p.add_argument('--render',action='store_true');p.add_argument('--only-new',action='store_true');a=p.parse_args();root=Path(__file__).resolve().parents[1];host=Path(a.host).resolve()
catalog=json.loads((root/'examples/catalog.json').read_text());assert len(catalog)>=48
if a.only_new:catalog=[r for r in catalog if r['name'].startswith('web_')]
for row in catalog:assert (root/row['script']).is_file()
if not a.render:
 for row in catalog:
  image=root/row['image'];data=image.read_bytes();assert data[:8]==b'\x89PNG\r\n\x1a\n' and len(data)<=307200,image
 print('PASS gallery assets: '+str(len(catalog)));raise SystemExit(0)
env=dict(os.environ,LMCURL_LIVE='0');env['PATH']=str(host.parent)+':/home/mma/repos/dsl/lmplot/build:/home/mma/repos/dsl/lmdraw/build:'+env['PATH']
compiler=subprocess.check_output([str(root/'scripts/select-compiler.sh')],text=True,env=env,timeout=30).strip()
fail=[]
for row in catalog:
 with tempfile.TemporaryDirectory(prefix='lmcurl-example-') as td:
  r=subprocess.run([compiler,str(root/row['script'])],cwd=td,env=env,text=True,capture_output=True,timeout=180)
  if r.returncode:
   print('FAIL '+row['name']+': '+r.stdout[-400:]+r.stderr[-700:],flush=True);fail.append(row['name']);continue
  image=Path(td)/'images'/(row['name']+'.png');data=image.read_bytes();assert data[:8]==b'\x89PNG\r\n\x1a\n' and len(data)<=307200,image
  assert struct.unpack('>II',data[16:24])==(900,560),row['name']
  if row['name'].startswith('web_'):
   ledger=json.loads((Path(td)/'reports'/(row['name']+'-cost.json')).read_text())
   assert ledger['estimated_cost_usd']==0 and ledger['requests']>0 and ledger['hard_run_cap_usd']==0.25
   requests=[json.loads(line) for line in (Path(td)/'data/web-requests.jsonl').read_text().splitlines() if line]
   assert len(requests)==ledger['requests'] and all(v['route'].startswith('/site/') and v['estimated_cost_usd']==0 for v in requests)
   if row['name']=='web_robots_crawler':assert all(not v['route'].startswith('/site/private/') for v in requests)
  (root/row['image']).write_bytes(data)
  print('PASS '+row['name'],flush=True)
if fail:raise SystemExit('failed examples: '+','.join(fail))
print('PASS all '+str(len(catalog))+' self-executing examples rendered from empty directories')
