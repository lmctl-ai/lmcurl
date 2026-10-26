#!/usr/bin/env python3
"""Explicit tiny provider probes. All credential values stay in the process environment."""
import argparse,json,os,re,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--host',default='build/lmcurl');p.add_argument('--provider');p.add_argument('--retry-failed',action='store_true');a=p.parse_args();host=Path(a.host).resolve()
if os.getenv('LMCURL_LIVE')!='1':raise SystemExit('Set LMCURL_LIVE=1 explicitly')
if os.getenv('CI','').lower() in ('true','1') or os.getenv('GITHUB_ACTIONS')=='true':raise SystemExit('Live probes forbidden in CI')
keys=[v for n in ('OPENROUTER_API_KEY','DEEPSEEK_API_KEY','OLLAMA_API_KEY','GITHUB_TOKEN') if (v:=os.getenv(n))]
def safe(text):
 for k in keys:text=text.replace(k,'[REDACTED]')
 return text
work=root/'build-vendored2/live';work.mkdir(parents=True,exist_ok=True)
specs=[('openrouter','openrouter/free','llm.openrouter("openrouter/free")'),('deepseek','deepseek-flash','llm.deepseek("deepseek-flash")'),('ollama-local','llama3.2:3b','llm.provider("ollama",curl.env("OLLAMA_HOST"),"llama3.2:3b")'),('ollama-cloud','gpt-oss:20b','llm.provider("ollama","https://ollama.com/api","gpt-oss:20b")')]
proof_path=root/'evidence/live-llm-v010.json';prior=json.loads(proof_path.read_text()) if proof_path.exists() else {};results=prior.get('provider_results',[]);total=float(prior.get('total_estimated_cost_usd',0))
env=dict(os.environ,LMCURL_LIVE='1',LMCURL_MAX_COST_USD='0.02');env['PATH']=str(host.parent)+os.pathsep+env['PATH']
compiler=subprocess.check_output([str(root/'scripts/select-compiler.sh')],env=env,text=True,timeout=30).strip()
for name,model,provider in specs:
 if a.provider and a.provider!=name:continue
 previous=next((v for v in results if v['provider']==name),None)
 if previous and (not a.retry_failed or previous['status']=='passed'):continue
 if total+0.02>0.25:raise SystemExit('aggregate probe cap refuses next provider')
 journal=work/(name+'-cost.jsonl');resultfile=work/(name+'-result.json');summary=work/(name+'-ledger.json');cassette=work/(name+'-cassette.json')
 if journal.exists():
  if not a.retry_failed or not previous or previous['status']=='passed':raise SystemExit('refusing to repeat successful live probe')
  journal=work/(name+'-retry-cost.jsonl')
  if journal.exists():raise SystemExit('one failed-probe retry already used')
 source=work/(name+'.lms');bytecode=work/(name+'.dslb')
 source.write_text('p = _dsl_ cli="lmcurl";\nlet result='+provider+'.chat("Reply with OK.").max_tokens(16).timeout(120000).cassette('+json.dumps(str(cassette))+').send();\nfile_write('+json.dumps(str(resultfile))+',result.json());\nllm.ledger('+json.dumps(str(summary))+');\nend _dsl_\nsavedsl(p,'+json.dumps(str(bytecode))+');\n')
 env2=dict(env,LMCURL_COST_LEDGER=str(journal));
 if name=='ollama-local':env2['LMCURL_RESPONSE_TRACE']=str(work/'ollama-local-wire.json')
 compile_result=subprocess.run([compiler,str(source)],cwd=root,env=env2,text=True,capture_output=True,timeout=30)
 if compile_result.returncode:
  row={'provider':name,'requested_model':model,'status':'compile_failed','estimated_cost_usd':0};results.append(row);print(name+': compile_failed');continue
 run=subprocess.run([str(host),'--dsl',str(bytecode)],cwd=root,env=env2,text=True,capture_output=True,timeout=150)
 # Never persist raw stdout/stderr; journal and normalized result are redacted by the host.
 events=[json.loads(l) for l in journal.read_text().splitlines()] if journal.exists() else []
 settled=[v for v in events if v.get('event')=='settled'];reserved=[v for v in events if v.get('event')=='reserved'];amount=sum(float(v['estimated_cost_usd']) for v in settled)
 if len(reserved)>len(settled):amount+=sum(float(v['reserved_usd']) for v in reserved[len(settled):])
 total+=amount
 result=json.loads(resultfile.read_text()) if resultfile.exists() else {}
 row={'provider':name,'requested_model':model,'model':result.get('model',model),'status':'passed' if run.returncode==0 else 'failed','http_status':result.get('status',settled[-1].get('status',0) if settled else 0),'input_tokens':result.get('input_tokens',settled[-1].get('input_tokens',0) if settled else 0),'output_tokens':result.get('output_tokens',settled[-1].get('output_tokens',0) if settled else 0),'estimated_cost_usd':amount,'attempts':len(reserved),'assertions':{'completion_received':run.returncode==0,'nonempty_text':bool(result.get('text')),'token_usage':bool(result.get('input_tokens',0) or result.get('output_tokens',0))},'price_basis':settled[-1].get('price_basis','no request reached') if settled else 'no request reached'}
 if run.returncode:row['failure_category']='request failed or provider unavailable'
 results=[v for v in results if v['provider']!=name];
 if previous:row['previous_attempts']=[previous]
 results.append(row);print(safe(name+': '+row['status']+'; model '+str(row['model'])+'; tokens '+str(row['input_tokens'])+'/'+str(row['output_tokens'])+'; estimated $'+format(amount,'.8f')),flush=True)
 proof={'live_opt_in':True,'default_example_cap_usd':0.02,'hard_run_cap_usd':0.25,'provider_results':results,'total_estimated_cost_usd':total,'secrets_recorded':False,'pricing_note':'DeepSeek uses conservative peak/cache-miss table rates; OpenRouter free variants are zero; local/Ollama subscription marginal prices exclude infrastructure/subscription allocation.'}
 text=json.dumps(proof,indent=2)+'\n';assert not any(k in text for k in keys)
 (root/'evidence').mkdir(exist_ok=True);(root/'evidence/live-llm-v010.json').write_text(text)
assert total<=0.25
print('total estimated $'+format(total,'.8f')+'; no keys printed')
