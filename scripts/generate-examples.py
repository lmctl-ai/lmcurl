#!/usr/bin/env python3
"""Generate self-contained offline stories; all chart values come from host execution."""
from pathlib import Path
import json
root=Path(__file__).resolve().parents[1]
for p in (root/'examples').glob('[0-9][0-9]_*.lms'):p.unlink()
S=[]
def add(name,title,body,metrics,pre='',plot=None):S.append((name,title,body,metrics,pre,plot))
add('rest_crud','A user record through its complete lifecycle', '''let a=s.post("/users").json("{\\"name\\":\\"Ada\\"}").send();
assert_eq(a.status(),201);s.set("id",a.json("$.id").value());
let b=s.put("/users/{{id}}").json("{\\"name\\":\\"Grace\\"}").send();assert_eq(b.json("$.name").value(),"Grace");
let c=s.get("/users/{{id}}").send();assert_eq(c.status(),200);
let d=s.delete("/users/{{id}}").send();assert_eq(d.json("$.deleted").value(),"true");
let missing=s.get("/users/{{id}}").send();assert_eq(missing.status(),404);''',[('create HTTP','a.status()'),('update HTTP','b.status()'),('read HTTP','c.status()'),('after delete','missing.status()')])
add('login_session','Login capture unlocks a protected resource','''let suite=curl.suite("login capture");
suite.test_request("login",s.post("/login").json("{\\"username\\":\\"demo\\",\\"password\\":\\"demo\\"}")).expect_status(200).capture_secret("token","$.token");
suite.test_request("protected",s.get("/protected").header("Authorization","Bearer {{token}}")).expect_status(200).expect_json("$.authorized","true");
let report=suite.report();assert_eq(report.failed(),0);''',[('passed checks','report.passed()'),('failed checks','report.failed()')])
add('cookie_jar','A session keeps cookies across requests','''let set=s.get("/cookies/set?theme=dark").send();assert_eq(set.cookie("theme"),"dark");
let r=s.get("/cookies").send();assert_eq(r.json("$.cookies.theme").value(),"dark");
let fresh=curl.session(m.url()).get("/cookies").send();assert_eq(fresh.json("$.cookies.theme").exists(),false);''',[('session cookies','r.json("$.cookies").count()'),('fresh client cookies','fresh.json("$.cookies").count()')])
add('auth_boundary','Anonymous and authenticated access','''let anon=s.get("/auth/basic").send();let auth=s.get("/auth/basic").basic("demo","demo").send();
assert_eq(anon.status(),401);assert_eq(auth.status(),200);''',[('anonymous status','anon.status()'),('authenticated status','auth.status()')])
add('redirect_chain','Following a bounded redirect chain','''let held=s.get("/redirect/3").follow(false).send();let followed=s.get("/redirect/3").send();
assert_eq(held.status(),302);assert_eq(followed.status(),200);assert_eq(followed.json("$.redirected").value(),"true");''',[('without follow','held.status()'),('followed status','followed.status()')])
add('retry_recovery','Recovery after three transient failures','''let r=s.get("/flaky/3").retry(3,5).send();assert_eq(r.status(),200);assert_eq(r.attempts(),4);''',[('attempts','r.attempts()'),('server attempt','r.json("$.attempt").to_int()')])
add('poll_readiness','Poll assertions until the service is ready','''let suite=curl.suite("readiness");suite.test_request("ready",s.get("/poll/2")).expect_status(200).poll_json("$.ready","true",3,5);
let report=suite.report();assert_eq(report.failed(),0);let attempts=curl.query_json(report.json(),"$.cases[0].attempts").to_int();assert_eq(attempts,3);''',[('poll attempts','attempts'),('passed cases','report.passed()')])
add('json_contract','Required fields and collection bounds','''let r=s.get("/json").send();
let valid=r.assert_json_schema("{\\"type\\":\\"object\\",\\"required\\":[\\"ok\\",\\"items\\"],\\"properties\\":{\\"items\\":{\\"type\\":\\"array\\",\\"minItems\\":3}}}");
let invalid=r.assert_json_schema("{\\"type\\":\\"object\\",\\"required\\":[\\"missing\\"]}");assert_eq(valid,true);assert_eq(invalid,false);
let good=0;if(valid){good=1;}let bad=0;if(invalid){bad=1;}''',[('valid contract','good'),('missing required field','bad'),('items observed','r.json("$.items").count()')])
add('jsonpath_projection','Typed JSONPath projections and predicates','''let r=s.get("/json").send();let items=r.json("$.items[*]");assert_eq(items.count(),3);assert_eq(items.nth(1).to_int(),2);
assert_eq(r.json("$.ok").exists(),true);assert_eq(r.json("$.missing").exists(),false);''',[('user id','r.json("$.user.id").to_int()'),('item count','items.count()'),('second item','items.nth(1).to_int()')])
add('xml_catalog','XPath checks on an XML catalog','''let r=s.get("/xml").send();assert_eq(r.xpath("string(/catalog/item[1])").value(),"alpha");let count=r.xpath("count(/catalog/item)").value();assert_eq(curl.query_text(count).gt("1"),true);''',[('catalog items','str_to_float(count)'),('HTTP status','r.status()')])
add('html_extract','Extracting HTML with XPath','''let r=s.get("/html").send();assert_eq(r.xpath("string(//h1)").value(),"lmcurl");let count=r.xpath("count(//p)").value();assert_eq(curl.query_text(count).gt("1"),true);''',[('paragraphs','str_to_float(count)'),('heading characters','str_len(r.xpath("string(//h1)").value())')])
add('csv_cases','Three CSV rows exercise one request template','''let suite=curl.suite("CSV cases").data_csv("data/cases.csv");suite.test_request("echo row",s.get("/echo?id={{id}}")).expect_status(200).expect_json("$.args.id","{{id}}");let report=suite.report();assert_eq(report.passed(),3);assert_eq(report.failed(),0);''',[('rows passed','report.passed()'),('rows failed','report.failed()')],pre='write_file("data/cases.csv", "id\\na\\nb\\nc\\n");')
add('json_cases','JSON data drives a reusable request','''let suite=curl.suite("JSON cases").data_json("data/cases.json");suite.test_request("echo",s.get("/echo?id={{id}}")).expect_status(200).expect_json("$.args.id","{{id}}");let report=suite.report();assert_eq(report.passed(),4);''',[('JSON rows','report.passed()'),('failed rows','report.failed()')],pre='write_file("data/cases.json", "[{\\"id\\":1},{\\"id\\":2},{\\"id\\":3},{\\"id\\":4}]");')
add('tag_filter','Select only the smoke contract','''let suite=curl.suite("tag selection").filter("smoke");suite.test_request("smoke",s.get("/json")).tag("smoke").expect_status(200);suite.test_request("other",s.get("/status/500")).tag("slow").expect_status(200);let report=suite.report();assert_eq(report.passed(),1);assert_eq(report.skipped(),1);''',[('selected passed','report.passed()'),('skipped by tag','report.skipped()'),('selected failed','report.failed()')])
add('parallel_checks','Independent checks run concurrently','''let suite=curl.suite("parallel").parallel(true);for(i in range(0,6)){suite.test_request("worker "+str(i),s.get("/delay/20")).expect_status(200);}let report=suite.report();assert_eq(report.passed(),6);assert_eq(report.failed(),0);report.csv("reports/parallel.csv");''',[('concurrent cases passed','report.passed()'),('failed cases','report.failed()')])
add('failure_reports','A failed contract produces actionable reports','''let suite=curl.suite("negative contract");suite.test_request("wrong status",s.get("/status/503")).expect_status(200);suite.test_request("wrong body",s.get("/json")).expect_body("not present");let report=suite.report();assert_eq(report.failed(),2);report.junit("reports/failures.xml");report.html("reports/failures.html");report.save("reports/failures.json");file_write("reports/failures.tap",report.tap());''',[('failures detected','report.failed()'),('unexpected passes','report.passed()')])
add('suite_lifecycle','Setup runs before protected checks','''let suite=curl.suite("lifecycle").before(s.post("/login").json("{\\"username\\":\\"demo\\",\\"password\\":\\"demo\\"}")).after(s.get("/echo?teardown=yes"));suite.test_request("protected",s.get("/protected")).expect_status(200);let report=suite.report();assert_eq(report.failed(),0);assert_eq(report.passed(),3);''',[('setup test teardown passed','report.passed()'),('failed stages','report.failed()')])
add('form_and_upload','Form encoding and multipart upload','''let form=s.post("/echo").form("name","Ada Lovelace").send();assert_eq(form.json("$.form.name").value(),"Ada Lovelace");let upload=s.post("/echo").part("note","fixture note").upload("artifact","data/upload.txt","sample.txt").send();assert_eq(str_contains(upload.body(),"upload fixture"),true);''',[('form status','form.status()'),('upload status','upload.status()'),('fixture bytes','str_len(file_read("data/upload.txt"))')],pre='write_file("data/upload.txt", "upload fixture");')
add('sse_events','Event-stream data stays bounded and inspectable','''let r=s.get("/sse").stream(true).send();assert_eq(r.status(),200);assert_eq(r.sse_count(),2);assert_eq(str_contains(r.body(),"[DONE]"),true);''',[('data events','r.sse_count()'),('payload bytes','r.bytes()')])
add('compression','Compressed responses are decoded before queries','''let r=s.get("/gzip").send();assert_eq(r.status(),200);assert_eq(r.json("$.compressed").value(),"true");assert_eq(r.json("$.name").value(),"lmcurl");''',[('decoded bytes','r.bytes()'),('name length','str_len(r.json("$.name").value())')])
add('response_limits','Oversized responses fail their contract','''let suite=curl.suite("response cap");suite.test_request("bounded response",s.get("/bytes/1024").limit(64)).expect_status(200);let report=suite.report();assert_eq(report.failed(),1);''',[('size failures detected','report.failed()'),('unexpected passes','report.passed()')])
add('snapshot_contract','Snapshots detect response regressions','''let r=s.get("/json").send();assert_eq(r.snapshot("data/json.snapshot",true),true);let same=s.get("/json").send();assert_eq(same.snapshot("data/json.snapshot",false),true);let different=s.get("/echo").send();assert_eq(different.snapshot("data/json.snapshot",false),false);let match=1;let mismatch=1;''',[('matching responses','match'),('different responses rejected','mismatch')])
add('openapi_contract','Check a JSON OpenAPI response contract','''let r=s.get("/json").send();let spec="{\\"openapi\\":\\"3.0.3\\",\\"paths\\":{\\"/json\\":{\\"get\\":{\\"responses\\":{\\"200\\":{\\"content\\":{\\"application/json\\":{\\"schema\\":{\\"type\\":\\"object\\",\\"required\\":[\\"ok\\",\\"items\\"]}}}}}}}}}}";assert_eq(r.assert_openapi(spec,"/json","GET"),true);assert_eq(r.assert_openapi(spec,"/missing","GET"),false);let accepted=1;let rejected=1;''',[('matching route','accepted'),('unknown route rejected','rejected')])
add('load_latency','Virtual users and latency thresholds','''let one=curl.load(s.get("/json"),1,1.0,20);let two=curl.load(s.get("/json"),2,1.0,20);let four=curl.load(s.get("/json"),4,1.0,20);assert_eq(one.threshold(2000.0,0.0),true);assert_eq(two.threshold(2000.0,0.0),true);assert_eq(four.threshold(2000.0,0.0),true);''',[('one user p95 ms','one.p95_ms()'),('two users p95 ms','two.p95_ms()'),('four users p95 ms','four.p95_ms()')])
add('llm_prompt','A prompt regression is an ordinary assertion','''let result=llm.mock("fixture-model").chat("ping").max_tokens(8).send();assert_eq(result.equals("pong"),true);assert_eq(result.not_contains("error"),true);''',[('input fixture tokens','result.input_tokens()'),('output fixture tokens','result.output_tokens()'),('output bytes','result.length()')])
add('llm_streams','Both OpenAI and Ollama streaming fixtures','''let a=llm.provider("mock",m.url()+"/v1","fixture-model").chat("hello").stream(true).send();let b=llm.provider("ollama",m.url(),"fixture-model").chat("hello").stream(true).send();assert_eq(a.contains("fixture"),true);assert_eq(b.contains("fixture"),true);assert_eq(a.output_tokens(),3);assert_eq(b.output_tokens(),3);''',[('OpenAI output tokens','a.output_tokens()'),('Ollama output tokens','b.output_tokens()')])
add('llm_sampling','Sampling reports measured prompt pass rates','''let sample=llm.mock("fixture").chat("ping").sample(5,1.0);assert_eq(sample.len(),5);assert_close(sample.pass_rate("^pong$"),1.0,0.0001);''',[('samples','sample.len()'),('pass percentage','sample.pass_rate("^pong$")*100.0'),('missing text percentage','sample.pass_rate("missing")*100.0')])
add('llm_matrix','A model matrix compares one prompt side by side','''let matrix=llm.mock("fixture").matrix("fixture-a,fixture-b,fixture-c","ping");assert_eq(matrix.len(),3);assert_close(matrix.pass_rate("pong"),1.0,0.0001);matrix.csv("data/model_matrix.csv");''',[('fixture a output tokens','matrix.at(0).output_tokens()'),('fixture b output tokens','matrix.at(1).output_tokens()'),('fixture c output tokens','matrix.at(2).output_tokens()')])
add('llm_replay','Recorded completions replay without credentials','''let provider=llm.mock("fixture");let first=provider.chat("ping").max_tokens(8).cassette("data/prompt.cassette.json").send();let replay=provider.chat("ping").max_tokens(8).replay("data/prompt.cassette.json").send();assert_eq(first.text(),replay.text());assert_close(replay.estimated_cost(),0.0,0.00001);llm.ledger("reports/cost.json");''',[('recorded output tokens','first.output_tokens()'),('replayed output tokens','replay.output_tokens()')])
add('llm_json','Structured completions have schema contracts','''let result=llm.mock("fixture").chat("Reply as JSON").json_mode(true).send();assert_eq(result.json_valid(),true);assert_eq(result.schema_valid("{\\"type\\":\\"object\\",\\"required\\":[\\"ok\\",\\"answer\\"]}"),true);''',[('answer','curl.query_json(result.text(),"$.answer").to_int()'),('fixture output tokens','result.output_tokens()')])
add('timing_breakdown','Where a delayed response spends its time','''let r=s.get("/delay/40").send();assert_eq(r.status(),200);assert(r.time("total")>=r.time("ttfb"));assert(r.time("total")>=30.0);''',[('DNS ms','r.time("dns")'),('connect ms','r.time("connect")'),('TLS ms','r.time("tls")'),('first byte ms','r.time("ttfb")'),('total ms','r.time("total")')])
add('secret_redaction','Credentials stay out of saved artifacts','''let r=s.post("/login").json("{\\"username\\":\\"demo\\",\\"password\\":\\"demo\\"}").send();r.save("reports/login.json");let saved=file_read("reports/login.json");let masked=0;let leaked=0;if(str_contains(saved,"[REDACTED]")){masked=1;}if(str_contains(saved,"fixture-token")){leaked=1;}assert_eq(masked,1);assert_eq(leaked,0);''',[('redaction verified','masked'),('credential leaks','leaked')])
# Build nested contracts as JSON objects, avoiding hand-counted escaped braces.
for i,row in enumerate(S):
    if row[0]=='openapi_contract':
        contract={"openapi":"3.0.3","paths":{"/json":{"get":{"responses":{"200":{"content":{"application/json":{"schema":{"type":"object","required":["ok","items"]}}}}}}}}}
        body='let r=s.get("/json").send();let spec='+json.dumps(json.dumps(contract))+';assert_eq(r.assert_openapi(spec,"/json","GET"),true);assert_eq(r.assert_openapi(spec,"/missing","GET"),false);let accepted=1;let rejected=1;'
        S[i]=(row[0],row[1],body,*row[3:])
for name,title,body,metrics,pre,plot in S:
    code='mkdir("data"); mkdir("images"); mkdir("reports"); mkdir("build/examples"); mkdir("docs/images");\n'+pre+'\n'
    code+='p = _dsl_ cli="lmcurl" install="github:lmctl-ai/lmcurl@latest";\nlet m=curl.mock();\nlet s=curl.session(m.url());\n'+body+'\n'
    if name=='llm_matrix':
        code+='let heat="x,y,pass\\n";\nfor(i in range(0,matrix.len())){let a=0;let b=0;if(matrix.at(i).contains("pong")){a=1;}if(matrix.at(i).json_valid()){b=1;}heat=heat+"0,"+str(i)+","+str(a)+"\\n1,"+str(i)+","+str(b)+"\\n";}\nfile_write("data/model_heatmap.csv",heat);\n'
    code+='let csv="metric,value\\n";\n'
    for label,expr in metrics:code+='csv=csv+'+json.dumps(label+',')+'+str('+expr+')+"\\n";\n'
    code+='file_write("data/'+name+'.csv",csv);\nm.stop();\nend _dsl_\n'
    code+='savedsl(p,"build/examples/'+name+'.dslb");\nr = system ${dsl_cli(p)} --dsl build/examples/'+name+'.dslb;\nif(r.code != 0){println(r.stdout);println(r.stderr);}\nassert(r.code==0);\n'
    code+='plot = _dsl_ cli="lmplot" install="github:lmctl-ai/lmplot@latest";\nplot.png("images/'+name+'.png",900,560);\n'
    for cmd in ["set datafile separator ','",'set title '+json.dumps(title),'set yrange [0:*]; set offsets 0,0,graph 0.12,0; set grid y; unset key; set style fill solid 0.85; set boxwidth 0.65',"set xtics rotate by -15 font ',10'",'set ylabel '+json.dumps('observed value'),"plot 'data/"+name+".csv' every ::1 using 0:2:xtic(1) with boxes lc rgb '#0969da', '' every ::1 using 0:2:(sprintf('%.3g',$2)) with labels offset 0,0.8"]:code+='plot.cmd('+json.dumps(cmd)+');\n'
    code+='plot.close();\nend _dsl_\nsavedsl(plot,"build/examples/'+name+'-plot.dslb");\nr = system ${dsl_cli(plot)} --dsl build/examples/'+name+'-plot.dslb;\nassert(r.code==0);\nassert(copy_file("images/'+name+'.png","docs/images/'+name+'.png"));\nprintln("images/'+name+'.png");\n'
    (root/'examples'/f'{name}.lms').write_text(code)
(root/'examples/catalog.json').write_text(json.dumps([{'name':n,'title':t,'script':f'examples/{n}.lms','image':f'docs/images/{n}.png'} for n,t,*_ in S],indent=2)+'\n')
print('generated',len(S),'self-contained stories')

p=root/'examples/llm_matrix.lms'
s=p.read_text();a=s.index('plot.cmd(');b=s.index('plot.close();',a)
commands=["set datafile separator ','", "set title 'Prompt contract matrix (offline fixtures)'", "set xrange [-0.5:1.5]; set yrange [-0.5:2.5]; unset key", "set xtics ('contains pong' 0, 'valid JSON' 1); set ytics ('fixture a' 0, 'fixture b' 1, 'fixture c' 2)", "set palette defined (0 '#cf222e', 1 '#1a7f37'); set cbrange [0:1]; set cbtics ('FAIL' 0, 'PASS' 1)", "plot 'data/model_heatmap.csv' every ::1 using 1:2:3 with image, '' every ::1 using 1:2:(sprintf('%d',$3)) with labels tc rgb '#ffffff' font ',16'"]
s=s[:a]+''.join('plot.cmd('+json.dumps(c)+');\n' for c in commands)+s[b:];p.write_text(s)
# The login story also documents the verified request sequence as a flow diagram.
p=root/'examples/login_session.lms'
s=p.read_text();s=s[:s.index('plot = _dsl_')]
s+='''draw = _dsl_ cli="lmdraw" install="github:lmctl-ai/lmdraw@latest";
scene.canvas(900,560,"#0d1726");
scene.text(48.0,58.0,26.0,"Login, capture, then protected request","#edf5ff");
scene.text(48.0,91.0,15.0,"Two real mock-server requests; assertions must pass before rendering","#a9bdd5");
let a=scene.rect(70.0,150.0,310.0,110.0,"#17344c");a.set_attr("rx","12");
scene.text(92.0,188.0,21.0,"POST /login","#ffffff");
scene.text(92.0,220.0,16.0,"demo credentials -> HTTP 200","#b6e3ff");
let b=scene.rect(520.0,150.0,310.0,110.0,"#17344c");b.set_attr("rx","12");
scene.text(542.0,188.0,21.0,"Capture $.token","#ffffff");
scene.text(542.0,220.0,16.0,"Register as a secret variable","#b6e3ff");
let c=scene.rect(520.0,360.0,310.0,110.0,"#17344c");c.set_attr("rx","12");
scene.text(542.0,398.0,21.0,"GET /protected","#ffffff");
scene.text(542.0,430.0,16.0,"Bearer {{token}} + session cookie","#b6e3ff");
let d=scene.rect(70.0,360.0,310.0,110.0,"#174638");d.set_attr("rx","12");
scene.text(92.0,398.0,21.0,"Assert authorized = true","#ffffff");
scene.text(92.0,430.0,16.0,"Report: two passed, zero failed","#aff2ce");
let e1=scene.line(390.0,205.0,505.0,205.0,"#79c0ff",3.0);e1.set_attr("marker-end","arrow");
let e2=scene.line(675.0,272.0,675.0,344.0,"#79c0ff",3.0);e2.set_attr("marker-end","arrow");
let e3=scene.line(507.0,415.0,395.0,415.0,"#79c0ff",3.0);e3.set_attr("marker-end","arrow");
scene.text(48.0,520.0,14.0,"Fixture secrets are redacted from exported artifacts. No cloud credentials required.","#a9bdd5");
scene.render_png("images/login_session.png",900,560,1);
end _dsl_
savedsl(draw,"build/examples/login_session-draw.dslb");
r = system ${dsl_cli(draw)} --dsl build/examples/login_session-draw.dslb;
assert(r.code==0);
assert(copy_file("images/login_session.png","docs/images/login_session.png"));
println("images/login_session.png");
'''
p.write_text(s)

# Keep the new standalone downloader/parser catalog alongside the original stories.
import subprocess,sys
subprocess.run([sys.executable,str(root/"scripts/generate-web-examples.py")],check=True)
