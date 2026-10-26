#!/usr/bin/env python3
"""Generate standalone downloader/parser stories, never runtime repo-relative imports."""
import json, argparse
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--group',choices=['a','b','all'],default='all');args=parser.parse_args()
A=[];B=[]
if args.group in ('a','all'):
 from web_stories_a import STORIES as A
if args.group in ('b','all'):
 from web_stories_b import STORIES as B
root=Path(__file__).resolve().parents[1]
COMMON=r'''
fn check(ok, message) {
  if (!ok) { println(message); }
  assert(ok);
}
fn run_program(program, path) {
  savedsl(program, path);
  r = system ${dsl_cli(program)} --dsl ${path};
  if (r.code != 0) { println(r.stderr); }
  check(r.code == 0, "native stage failed: " + path);
  return r;
}
fn record_request(route, status, bytes) {
  append_file("data/web-requests.jsonl", json_stringify({route: route, status: status, bytes: bytes, completed_ms: now(), estimated_cost_usd: 0}) + "\n");
}
fn download(base, route, out, follow) {
  # Only caller-owned loopback fixtures: discovered external URLs are out of scope.
  check(starts_with(base, "http://127.0.0.1:"), "expected loopback mock URL");
  if (starts_with(route,base+"/site/")) { route=replace(route,base,""); }
  check(starts_with(route, "/site/") && !contains(route, ".."), "fixture route outside scope");
  p = _dsl_ cli="lmcurl" install="github:lmctl-ai/lmcurl@latest";
let r=curl.session(${base}).get(${route}).proxy("").follow(${follow}).timeout(5000).limit(1048576).send();
r.save(${out});
file_write(${out + ".status"},str(r.status()));
file_write(${out + ".bytes"},str(r.bytes()));
file_write(${out + ".location"},r.header("location"));
end _dsl_
  run_program(p, out + ".dslb");
  status = json_parse(read_file(out + ".status"));
  bytes = json_parse(read_file(out + ".bytes"));
  record_request(route, status, bytes);
  return {status: status, bytes: bytes, path: out, location: read_file(out + ".location")};
}
fn serve_site() {
  p = _dsl_ cli="lmcurl" install="github:lmctl-ai/lmcurl@latest";
let site=curl.mock();
site.serve("build/web-ready.txt","build/web-stop.txt",60000);
end _dsl_
  run_program(p,"build/web-server.dslb");
  return true;
}
fn wait_and_run() {
  for (i in range(0,600)) {
    if (file_exists("build/web-ready.txt")) { break; }
    sleep_ms(10);
  }
  check(file_exists("build/web-ready.txt"),"mock did not become ready");
  base=trim(read_file("build/web-ready.txt"));
  story(base);
  write_file("build/web-stop.txt","stop\n");
  return true;
}
'''
PLOT=r'''
plot = _dsl_ cli="lmplot" install="github:lmctl-ai/lmplot@latest";
plot.png("images/@NAME@.png",900,560);
plot.cmd("set datafile separator ','");
plot.cmd(@TITLE@);
plot.cmd("set yrange [0:*]; set offsets 0,0,graph 0.12,0; set grid y; unset key; set style fill solid 0.85; set boxwidth 0.65");
plot.cmd("set xtics rotate by -15 font ',10'");
plot.cmd("set ylabel 'observed count / labeled value'");
plot.cmd("plot 'data/@NAME@.csv' every ::1 using 0:2:xtic(1) with boxes lc rgb '#0969da', '' every ::1 using 0:2:(sprintf('%.3g',$2)) with labels offset 0,0.8");
plot.close();
end _dsl_
run_program(plot,"build/@NAME@-plot.dslb");
'''
rows=[]
for row in A+B:
 name=row['name'];title=row['title'];story=row['story'].lstrip('\\\n')
 code='# '+title+'\n# Offline: lmcurl downloads; lmhtml parses; lmplot/lmdraw renders.\n'
 code+='mkdir("data"); mkdir("build"); mkdir("images"); mkdir("reports"); mkdir("docs/images");\n'
 code+='for (path in ["build/web-ready.txt","build/web-stop.txt"]) { if (file_exists(path)) { remove_file(path); } }\nwrite_file("data/web-requests.jsonl","");\n'
 code+=COMMON+'\nfn story(base) {\n'+story+'\n}\n'
 code+=r'''# Older published lmhtml binaries predate the implicit standard file catalog.
html_probe=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let doc=html.parse("<html><body>compatibility probe</body></html>");
end _dsl_
html_version=system ${dsl_cli(html_probe)} --version;
check(html_version.code==0,"cannot query lmhtml version");
version_words=split(trim(html_version.stdout)," ");
check(version_words.len()==2&&version_words[0]=="lmhtml","unexpected lmhtml version output");
version_parts=split(version_words[1],".");check(version_parts.len()==3,"unexpected lmhtml version number");
major=json_parse(version_parts[0]);minor=json_parse(version_parts[1]);patch=json_parse(version_parts[2]);
check(major>0||minor>7||(minor==7&&patch>=4),"lmhtml >=0.7.4 is required; upgrade or remove an older PATH installation");
parallel([serve_site(), wait_and_run()]);
'''
 code+='events=read_lines("data/web-requests.jsonl"); request_count=0;\nfor (line in events) { if (!is_blank(line)) { request_count=request_count+1; } }\n'
 code+='write_json("reports/'+name+'-cost.json",{mode:"offline-loopback",currency:"USD",price_table:{loopback_request_usd:0},example_cap_usd:0.02,hard_run_cap_usd:0.25,requests:request_count,estimated_cost_usd:0});\n'
 code+='println("estimated cost: $0.0000 (loopback fixtures only)");\n'
 if row.get('draw'):
  code+='draw = _dsl_ cli="lmdraw" install="github:lmctl-ai/lmdraw@latest";\n'+row['draw']+'\nend _dsl_\nrun_program(draw,"build/'+name+'-draw.dslb");\n'
 elif row.get('plot'):
  code+='plot = _dsl_ cli="lmplot" install="github:lmctl-ai/lmplot@latest";\nplot.png("images/'+name+'.png",900,560);\n'
  code+=''.join('plot.cmd('+json.dumps(cmd)+');\n' for cmd in row['plot'])
  code+='plot.close();\nend _dsl_\nrun_program(plot,"build/'+name+'-plot.dslb");\n'
 else:code+=PLOT.replace('@NAME@',name).replace('@TITLE@',json.dumps('set title '+json.dumps(title)))
 code+='assert(copy_file("images/'+name+'.png","docs/images/'+name+'.png"));\nprintln("images/'+name+'.png");\n'
 (root/'examples'/f'{name}.lms').write_text(code)
 rows.append({'name':name,'title':title,'script':f'examples/{name}.lms','image':f'docs/images/{name}.png','category':'web-downloader-parser'})
catalog=json.loads((root/'examples/catalog.json').read_text());catalog=[r for r in catalog if r['name'] not in {x['name'] for x in rows}]+rows
(root/'examples/catalog.json').write_text(json.dumps(catalog,indent=2)+'\n')
print('generated '+str(len(rows))+' downloader/parser stories')
