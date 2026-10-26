// Local, deterministic mock for the lmcurl tests (node, no dependencies). Prints `listening on PORT` and serves:
//   POST /echo                 -> JSON {method, path, headers (lowercased), body}
//   POST /v1/chat/completions  -> OpenAI-style chat completion; requires `Authorization: Bearer test-key`; the reply repeats
//                                 the last user message so the test can assert extraction; 401 without the key; 400 on bad JSON
//   GET  /go                   -> 302 to /site/index.html
//   GET  /chunked              -> a Transfer-Encoding: chunked text body (three chunks)
//   GET  /site/...             -> a small static site: relative/absolute links, a loop (a <-> b), an off-site link, a 404 link,
//                                 a depth chain (d1 -> d2 -> d3), a CSS file, a PNG pretending content type, ../ and ./ links
// Usage: node tools/mock-server.mjs [PORT]   (0 = free port). SIGTERM stops it.
import {createServer} from 'node:http';
const site={
 '/site/index.html':{type:'text/html',body:'<html><body><h1>Index</h1><a href="a.html">a</a> <a href="/site/dir/">dir</a> <a href="http://offsite.invalid/x.html">offsite</a> <a href="missing.html">missing</a> <a href="d1.html">deep</a> <a href="style.css">css</a> <a href="#top">anchor</a> <a href="a.html?v=2">query</a> <img src="img/logo.png"></body></html>'},
 '/site/a.html':{type:'text/html; charset=utf-8',body:'<html><body>A <a href="b.html">b</a> <a href="./index.html">home</a></body></html>'},
 '/site/b.html':{type:'text/html',body:'<html><body>B <a href="a.html">loop back to a</a> <a href="../site/dir/">dir again</a></body></html>'},
 '/site/dir/':{type:'text/html',body:'<html><body>Dir index <a href="../a.html">up to a</a> <a href="page.html">page</a></body></html>'},
 '/site/dir/page.html':{type:'text/html',body:'<html><body>Page <a href="../../site/b.html">b via dotdot</a></body></html>'},
 '/site/d1.html':{type:'text/html',body:'<html><body>Depth 1 <a href="d2.html">d2</a></body></html>'},
 '/site/d2.html':{type:'text/html',body:'<html><body>Depth 2 <a href="d3.html">d3</a></body></html>'},
 '/site/d3.html':{type:'text/html',body:'<html><body>Depth 3 (never reached at depth 2)</body></html>'},
 '/site/style.css':{type:'text/css',body:'body { color: #333; }'},
 '/site/img/logo.png':{type:'image/png',body:'\x89PNG not really'},
};
const server=createServer((req,res)=>{
 let body='';req.on('data',d=>body+=d);req.on('end',()=>{
  const url=new URL(req.url,'http://localhost');const path=url.pathname;
  const json=(code,obj)=>{const text=JSON.stringify(obj);res.writeHead(code,{'Content-Type':'application/json','Content-Length':Buffer.byteLength(text)});res.end(text);};
  if(path==='/echo'){const headers={};for(const [k,v] of Object.entries(req.headers))headers[k.toLowerCase()]=v;return json(200,{method:req.method,path:req.url,headers,body});}
  if(path==='/v1/chat/completions'){
   if(req.method!=='POST')return json(405,{error:{message:'POST only'}});
   if(req.headers.authorization!=='Bearer test-key')return json(401,{error:{message:'invalid api key'}});
   let parsed;try{parsed=JSON.parse(body);}catch{return json(400,{error:{message:'body is not JSON'}});}
   const last=(parsed.messages??[]).filter(m=>m.role==='user').at(-1)?.content??'';
   return json(200,{id:'chatcmpl-mock',object:'chat.completion',model:parsed.model??'mock',choices:[{index:0,message:{role:'assistant',content:'Echo: '+last},finish_reason:'stop'}],usage:{prompt_tokens:last.length,completion_tokens:6+last.length,total_tokens:6+2*last.length}});
  }
  if(path==='/go'){res.writeHead(302,{Location:'/site/index.html','Content-Length':'0'});return res.end();}
  if(path==='/chunked'){res.writeHead(200,{'Content-Type':'text/plain'});res.write('first ');setTimeout(()=>{res.write('second ');setTimeout(()=>res.end('third'),10);},10);return;}
  if(path==='/slow'){return setTimeout(()=>json(200,{slow:true}),3000);}
  if(path==='/big'){res.writeHead(200,{'Content-Type':'text/plain','Content-Length':'1000000'});return res.end(Buffer.alloc(1000000,65));}
  const page=site[path];
  if(page){res.writeHead(200,{'Content-Type':page.type,'Content-Length':Buffer.byteLength(page.body,'latin1')});return res.end(Buffer.from(page.body,'latin1'));}
  res.writeHead(404,{'Content-Type':'text/plain','Content-Length':'9'});res.end('not found');
 });
});
server.listen(Number(process.argv[2]??0),'127.0.0.1',()=>{console.log('listening on '+server.address().port);});
process.on('SIGTERM',()=>server.close(()=>process.exit(0)));process.on('SIGINT',()=>server.close(()=>process.exit(0)));
