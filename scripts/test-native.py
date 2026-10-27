#!/usr/bin/env python3
import argparse,json,os,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--host',required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1];host=Path(a.host).resolve()
env=dict(os.environ,LMCURL_LIVE='0');env['PATH']=str(host.parent)+os.pathsep+env['PATH']
compiler=subprocess.check_output([str(root/'scripts/select-compiler.sh')],env=env,text=True,timeout=30).strip();passed=0
with tempfile.TemporaryDirectory(prefix='lmcurl-native-') as td:
 work=Path(td)
 def run(body,code=0,extra=None):
  global passed
  env2=dict(env,**(extra or {}));source=work/'case.lms';out=work/'case.dslb'
  source.write_text('p = _dsl_ cli="lmcurl";\n'+body+'\nend _dsl_\nsavedsl(p,'+json.dumps(str(out))+');\n')
  r=subprocess.run([compiler,str(source)],cwd=work,env=env2,capture_output=True,text=True,timeout=30)
  assert r.returncode==0,'compile failed: '+r.stderr
  r=subprocess.run([str(host),'--dsl',str(out)],cwd=work,env=env2,capture_output=True,text=True,timeout=40)
  assert r.returncode==code,'host exit mismatch: '+str(r.returncode)+' '+r.stdout+' '+r.stderr
  passed+=1;return r
 run('let m=curl.mock(); let s=curl.session(m.url()); assert_eq(s.get("/json").send().status(),200); assert_eq(s.post("/echo").form("a","x y").send().json("$.form.a").value(),"x y");m.stop();')
 run('let m=curl.mock();let s=curl.session(m.url());s.get("/cookies/set?theme=dark").send();assert_eq(s.get("/cookies").send().json("$.cookies.theme").value(),"dark");assert_eq(s.get("/auth/basic").basic("demo","demo").send().status(),200);m.stop();')
 run('let m=curl.mock();let s=curl.session(m.url());assert_eq(s.get("/flaky/2").retry(2,1).send().status(),200);assert_eq(s.get("/redirect/2").send().status(),200);assert_eq(s.get("/redirect/2").follow(false).send().status(),302);m.stop();')
 run('let m=curl.mock();let r=curl.session(m.url()).get("/json").send();assert_eq(r.json("$.items").count(),3);assert_eq(r.json("$.items[1]").to_int(),2);assert_eq(r.json("$.ok").exists(),true);assert_eq(r.json("$.missing").exists(),false);assert_eq(r.assert_json_schema("{\\"required\\":[\\"missing\\"]}"),false);m.stop();')
 run('let m=curl.mock();let s=curl.session(m.url());let suite=curl.suite("negative");suite.test_request("wrong status",s.get("/status/404")).expect_status(200);suite.test_request("wrong body",s.get("/json")).expect_body("missing");let r=suite.report();assert_eq(r.failed(),2);r.junit("negative.xml");r.html("negative.html");file_write("negative.tap",r.tap());m.stop();')
 import xml.etree.ElementTree as ET
 x=ET.parse(work/'negative.xml').getroot();assert x.attrib['failures']=='2' and len(x.findall('testcase'))==2
 run('let m=curl.mock();let suite=curl.suite("exit");suite.test_request("bad",curl.session(m.url()).get("/status/500")).expect_status(200);suite.report().assert_all();m.stop();',1)
 run('let m=curl.mock();let s=curl.session(m.url());let suite=curl.suite("filter").filter("smoke");suite.test_request("yes",s.get("/json")).tag("smoke").expect_status(200);suite.test_request("no",s.get("/status/500")).tag("slow").expect_status(200);let r=suite.report();assert_eq(r.passed(),1);assert_eq(r.skipped(),1);m.stop();')
 (work/'cases.csv').write_text('id\na\nb\nc\n')
 run('let m=curl.mock();let s=curl.session(m.url());let suite=curl.suite("data").data_csv("cases.csv");suite.test_request("row",s.get("/echo?id={{id}}")).expect_json("$.args.id","{{id}}");assert_eq(suite.report().passed(),3);m.stop();')
 run('let m=curl.mock();let s=curl.session(m.url());let suite=curl.suite("hooks").before(s.post("/login").json("{\\"username\\":\\"demo\\",\\"password\\":\\"demo\\"}")).after(s.get("/echo"));suite.test_request("protected",s.get("/protected")).expect_status(200);assert_eq(suite.report().passed(),3);m.stop();')
 run('let m=curl.mock();let s=curl.session(m.url());let suite=curl.suite("parallel").parallel(true);for(i in range(0,4)){suite.test_request("case",s.get("/delay/5")).expect_status(200);}assert_eq(suite.report().passed(),4);let load=curl.load(s.get("/json"),2,0.5,10);assert_eq(load.requests(),10);assert_eq(load.error_rate(),0.0);m.stop();'.replace('assert_eq(load.error_rate(),0.0);','assert_close(load.error_rate(),0.0,0.0001);'))
 run('let m=curl.mock();curl.session(m.url()).get("/bytes/1024").limit(8).send();',3)
 run('curl.session("file:///tmp").get("/anything").send();',3)
 run('curl.session("http://127.0.0.1:1").get("/").header("X-Test","bad\\r\\nInjected: yes");',3)
 run('llm.deepseek("deepseek-flash").chat("ping").max_tokens(8).send();',3)
 run('llm.mock("fixture").chat("ping").max_tokens(129).send();',3)
 run('for(i in range(0,33)){llm.mock("fixture").chat("ping").send();}',3)
 secret='fixture-'+('q7Z9'*10)
 r=run('let value=curl.env("LMCURL_TEST_SECRET");print(value);file_write("redacted.txt",value);file_append("redacted.txt",value);',extra={'LMCURL_TEST_SECRET':secret})
 assert secret not in r.stdout and secret not in (work/'redacted.txt').read_text()
 run('let m=curl.mock();let r=curl.session(m.url()).get("/xml").send();assert_eq(r.xpath("string(/catalog/item[1])").value(),"alpha");m.stop();')
 run('let m=curl.mock();let r=curl.session(m.url()).get("/gzip").send();assert_eq(r.json("$.compressed").value(),"true");m.stop();')
 r=run('let s=curl.session("http://127.0.0.1:1");for(i in range(0,100000)){s.get("/").body(str_repeat("x",1000000));}',3,{'LMCURL_MAX_MEMORY_MB':'64'})
 assert 'LMCURL_MAX_MEMORY_MB' in r.stderr and 'memory budget exceeded' in r.stderr
 # Local servers keep all redirect regressions offline, even when guarding provider names.
 import http.server, threading
 class RedirectHandler(http.server.BaseHTTPRequestHandler):
  def do_POST(self): self.do_GET()
  def do_GET(self):
   if self.path=='/paid':
    self.send_response(307);self.send_header('Location','http://api.deepseek.com:1/chat/completions');self.end_headers()
   elif self.path=='/cross':
    self.send_response(307);self.send_header('Location',dest+'/echo');self.end_headers()
   elif self.path=='/literal':
    self.send_response(302);self.send_header('Location','/echo?value={{not_a_variable}}');self.end_headers()
   elif self.path=='/loop':
    self.send_response(307);self.send_header('Location','/loop');self.end_headers()
   else:
    data=json.dumps(dict(self.headers)).encode();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
  def log_message(self,*args): pass
 servers=[http.server.ThreadingHTTPServer(('127.0.0.1',0),RedirectHandler) for _ in range(2)]
 for server in servers: threading.Thread(target=server.serve_forever,daemon=True).start()
 origin='http://127.0.0.1:'+str(servers[0].server_port);dest='http://127.0.0.1:'+str(servers[1].server_port)
 try:
  r=run('curl.session('+json.dumps(origin)+').post("/paid").resolve("api.deepseek.com:1:127.0.0.1").send();',3)
  assert 'cost-controlled' in r.stderr
  run('let r=curl.session('+json.dumps(origin)+').header("Authorization","Bearer fixture-cross-origin").post("/cross").send();assert_eq(r.json("$.Authorization").exists(),false);')
  run('assert_eq(curl.session('+json.dumps(origin)+').get("/literal").send().status(),200);')
  r=run('curl.session('+json.dumps(origin)+').get("/loop").send();',3);assert 'redirect limit' in r.stderr
 finally:
  for server in servers:server.shutdown();server.server_close()
 deep='['*140+'0'+']'*140
 (work/'deep.json').write_text(deep)
 r=run('llm.mock("fixture").chat("ping").tool("deep",file_read("deep.json"));',3)
 assert 'nesting exceeds 128' in r.stderr
 run('llm.mock("fixture").chat("ping").send().matches(str_repeat("x",257));',3)
 run('assert_eq(curl.query_text(str_repeat("a",8000)+"!").matches("^(a+)+$"),false);')
 run('llm.mock("fixture").chat("ping").send().matches("[");',3)
 proxy_env={k:'http://127.0.0.1:1' for k in ('http_proxy','HTTP_PROXY','https_proxy','HTTPS_PROXY','ALL_PROXY','all_proxy')}
 proxy_env.update(NO_PROXY='',no_proxy='')
 run('let m=curl.mock();assert_eq(curl.session(m.url()).get("/json").proxy("").send().status(),200);m.stop();',extra=proxy_env)
 r=run('curl.session("http://127.0.0.1:1").get("/dead").timeout(500).send();',3)
 assert 'HTTP transport failed' in r.stderr
 r=run('curl.session("http://127.0.0.1:1").get("/dead").timeout(500).allow_error(false).send();',3)
 assert 'HTTP transport failed' in r.stderr
 run('let m=curl.mock();let bad=curl.session("http://127.0.0.1:1").get("/dead").timeout(500).allow_error(true).send();assert_eq(bad.status(),0);assert_eq(bad.bytes(),0);assert_eq(str_len(bad.error())>0,true);let ok=curl.session(m.url()).get("/json").allow_error(true).send();assert_eq(ok.status(),200);assert_eq(ok.error(),"");file_write("receipt.txt",str(bad.status())+"|"+bad.error()+"|"+str(ok.status()));m.stop();')
 assert (work/'receipt.txt').read_text().startswith('0|') and (work/'receipt.txt').read_text().endswith('|200')
print('PASS native HTTP/framework/LLM safety: '+str(passed)+' cases')
