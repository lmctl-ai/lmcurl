"""Download/parse/crawl recipes. Transport is lmcurl; document queries are lmhtml."""
STORIES=[]
def add(name,title,story,**options): STORIES.append(dict(name=name,title=title,story=story,**options))

add('web_link_checker','Check discovered site links and report missing or denied targets',r'''
seed=download(base,"/site/index.html","data/index.html",true);check(seed.status==200,"site seed");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let links=html.load("data/index.html").links();file_write("data/links.txt","");
for(i in range(0,links.len())){file_append("data/links.txt",links.at(i)+"\n");}
end _dsl_
run_program(p,"build/link-parser.dslb");
checked=0;healthy=0;missing=0;denied=0;report="url,status\n";
for(route in read_lines("data/links.txt")){
 if(is_blank(route)){continue;}r=download(base,route,"data/link-"+str(checked)+".html",false);checked=checked+1;
 if(r.status==200){healthy=healthy+1;}if(r.status==404){missing=missing+1;}if(r.status==403){denied=denied+1;}
 report=report+route+","+str(r.status)+"\n";
}
check(checked==4&&healthy==2&&missing==1&&denied==1,"link findings");
write_file("reports/broken-links.csv",report);
write_file("data/web_link_checker.csv","metric,value\nhealthy,"+str(healthy)+"\nnot found,"+str(missing)+"\naccess denied,"+str(denied)+"\n");
''')

add('web_sitemap_crawler','Parse an XML sitemap, scope its URLs, then crawl them',r'''
response=download(base,"/site/sitemap.xml","data/sitemap.xml",true);check(response.status==200,"sitemap fetch");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let locs=html.load("data/sitemap.xml").select("loc");file_write("data/sitemap-urls.txt","");
for(i in range(0,locs.len())){file_append("data/sitemap-urls.txt",locs.at(i).text()+"\n");}
end _dsl_
run_program(p,"build/sitemap-parser.dslb");
count=0;ok=0;failed=0;report="url,status\n";
for(url in read_lines("data/sitemap-urls.txt")){
 if(is_blank(url)){continue;}check(starts_with(url,base+"/site/"),"sitemap URL escaped the fixture origin");
 r=download(base,url,"data/sitemap-page-"+str(count)+".html",true);count=count+1;
 if(r.status==200){ok=ok+1;}else{failed=failed+1;}report=report+url+","+str(r.status)+"\n";
}
check(count==3&&ok==3&&failed==0,"sitemap crawl");write_file("reports/sitemap-crawl.csv",report);
write_file("data/web_sitemap_crawler.csv","metric,value\ndiscovered,"+str(count)+"\nfetched OK,"+str(ok)+"\nfailed,"+str(failed)+"\n");
''')

add('web_feed_reader','Read RSS and Atom headlines, publication dates and URLs',r'''
responses=parallel([download(base,"/site/feed.rss","data/feed.rss",true),download(base,"/site/feed.atom","data/feed.atom",true)]);
check(responses[0].status==200&&responses[1].status==200,"feed downloads");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
fn cell(s:string)->string{return "\""+str_replace(s,"\"","\"\"")+"\"";}
let rss=html.load("data/feed.rss").select("item");let atom=html.load("data/feed.atom").select("entry");
file_write("data/feed-table.csv","feed,title,published,url\n");
for(i in range(0,rss.len())){let x=rss.at(i);file_append("data/feed-table.csv","RSS,"+cell(x.select("title").at(0).text())+","+cell(x.select("pubDate").at(0).text())+","+cell(x.select("guid").at(0).text())+"\n");}
for(i in range(0,atom.len())){let x=atom.at(i);file_append("data/feed-table.csv","Atom,"+cell(x.select("title").at(0).text())+","+cell(x.select("updated").at(0).text())+","+cell(x.select("link").at(0).attr("href"))+"\n");}
assert_close(float(rss.len()),2.0,0.000001);assert_close(float(atom.len()),2.0,0.000001);
file_write("data/web_feed_reader.csv","metric,value\nRSS records,"+str(rss.len())+"\nAtom records,"+str(atom.len())+"\n");
end _dsl_
run_program(p,"build/feed-parser.dslb");
rows=read_csv("data/feed-table.csv");check(rows.len()==4,"feed table rows");
for(row in rows){check(!is_blank(row.title)&&!is_blank(row.published)&&starts_with(row.url,base+"/site/"),"feed record fields");}
check(rows[0].title=="Orbit launch"&&rows[1].title=="Comet watch","feed headlines");
''')

add('web_table_scraper','Scrape the product table into a price and stock CSV',r'''
r=download(base,"/site/products.html","data/products.html",true);check(r.status==200,"product table download");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let rows=html.load("data/products.html").select("table tr");let count=0;
file_write("data/products.csv","sku,price,stock\n");
for(i in range(0,rows.len())){let cells=rows.at(i).select("td");if(cells.len()==3){count=count+1;file_append("data/products.csv",cells.at(0).text()+","+cells.at(1).text()+","+cells.at(2).text()+"\n");}}
assert_close(float(count),3.0,0.000001);
file_write("data/web_table_scraper.csv","metric,value\nproducts,"+str(count)+"\n");
end _dsl_
run_program(p,"build/table-parser.dslb");rows=read_csv("data/products.csv");
check(rows.len()==3&&rows[0].sku=="orbit"&&json_parse(str(rows[0].price))==19.5&&json_parse(str(rows[1].stock))==0,"scraped values");
''',plot=["set datafile separator ','","set multiplot layout 1,2 title 'Scraped product catalog: prices and availability'","unset key; set style fill solid 0.85; set boxwidth 0.65; set grid y; set yrange [0:*]","set ylabel 'price (USD)'; set title 'Prices'","plot 'data/products.csv' every ::1 using 0:2:xtic(1) with boxes lc rgb '#0969da'","set ylabel 'stock (units)'; set title 'Availability'","plot 'data/products.csv' every ::1 using 0:3:xtic(1) with boxes lc rgb '#1a7f37'","unset multiplot"])

add('web_price_watcher','Poll until the price is at most 20 USD and stock is positive',r'''
write_file("data/watch-history.csv","poll,price,stock\n");attempts=0;price=100;stock=0;
for(i in range(0,5)){
 path="data/watch-"+str(i)+".html";r=download(base,"/site/watch.html",path,true);check(r.status==200,"price poll");
 p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let d=html.load(${path});
file_write("data/watch-price.txt",d.select(".price").at(0).text());file_write("data/watch-stock.txt",d.select(".stock").at(0).text());
end _dsl_
 run_program(p,"build/watch-parser-"+str(i)+".dslb");price=json_parse(read_file("data/watch-price.txt"));stock=json_parse(read_file("data/watch-stock.txt"));attempts=attempts+1;
 append_file("data/watch-history.csv",str(attempts)+","+str(price)+","+str(stock)+"\n");
 if(price<=20&&stock>0){break;}sleep_ms(20);
}
check(attempts==3&&price==19&&stock==3,"watch threshold");
write_file("data/web_price_watcher.csv","metric,value\npolls,"+str(attempts)+"\nfinal price,"+str(price)+"\nfinal stock,"+str(stock)+"\n");
''',plot=["set datafile separator ','","set multiplot layout 1,2 title 'Observed price/stock polls; buy threshold is 20 USD'","set key top right; set grid; set xrange [0.5:3.5]; set xtics 1; set xlabel 'poll'","set title 'Price'; set ylabel 'USD'; set yrange [15:28]","plot 'data/watch-history.csv' every ::1 using 1:2 with linespoints lw 3 pt 7 lc rgb '#0969da' title 'price', 20 with lines dt 2 lc rgb '#cf222e' title 'threshold'","set title 'Stock'; set ylabel 'units'; set yrange [0:4]; unset key; set style fill solid 0.85; set boxwidth 0.6","plot 'data/watch-history.csv' every ::1 using 1:3 with boxes lc rgb '#1a7f37'","unset multiplot"])

add('web_login_form','Parse the CSRF field, post credentials and retain the login cookie',r'''
r=download(base,"/site/login.html","data/login.html",true);check(r.status==200,"login form download");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let form=html.load("data/login.html").select("form").at(0);
file_write("data/csrf.txt",form.select("input[name=csrf_token]").at(0).attr("value"));
file_write("data/login-action.txt",form.attr("action"));
end _dsl_
run_program(p,"build/login-parser.dslb");csrf=read_file("data/csrf.txt");action=read_file("data/login-action.txt");
check(csrf=="fixture-csrf-public"&&action=="/site/login","fixture form fields");
# The public fixture token is handed off only to the downloader; cookie values never leave its process.
client=_dsl_ cli="lmcurl" install="github:lmctl-ai/lmcurl@latest";
let session=curl.session(${base});
let login=session.post(${action}).proxy("").form("csrf_token",${csrf}).form("username","demo").form("password","demo").send();
assert_eq(login.status(),200);
let account=session.get("/site/account.html").proxy("").send();assert_eq(account.status(),200);account.save("data/account.html");
file_write("data/login-status.txt",str(login.status()));file_write("data/login-bytes.txt",str(login.bytes()));
file_write("data/account-status.txt",str(account.status()));file_write("data/account-bytes.txt",str(account.bytes()));
end _dsl_
run_program(client,"build/login-client.dslb");remove_file("data/csrf.txt");
record_request(action,json_parse(read_file("data/login-status.txt")),json_parse(read_file("data/login-bytes.txt")));
record_request("/site/account.html",json_parse(read_file("data/account-status.txt")),json_parse(read_file("data/account-bytes.txt")));
account_parser=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let doc=html.load("data/account.html");
if(doc.select("h1").at(0).text()!="Demo account"){report.add("assertion","account",0,"wrong account title");report.fail_if_any();}
assert_close(str_to_float(doc.select(".balance").at(0).text()),42.0,0.000001);
file_write("data/web_login_form.csv","metric,value\nCSRF parsed,1\nlogin accepted,1\naccount verified,1\n");
end _dsl_
run_program(account_parser,"build/account-parser.dslb");
''')

add('web_pagination','Follow parsed next links to collect nine articles',r'''
route="/site/page/1.html";visited=[];pages=0;articles=0;
write_file("data/web_pagination.csv","metric,value\n");write_file("reports/article-headlines.csv","page,headline\n");
for(i in range(0,8)){
 check(!contains(visited,route),"pagination cycle");push(visited,route);
 path="data/page-"+str(i)+".html";r=download(base,route,path,true);check(r.status==200,"pagination download");
 p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let doc=html.load(${path});let entries=doc.select("article");let next=doc.select("a[rel=next]");
file_write("data/article-count.txt",str(entries.len()));file_write("data/next.txt","");
if(next.len()>0){file_write("data/next.txt",next.at(0).attr("href"));}
for(i in range(0,entries.len())){file_append("reports/article-headlines.csv",${str(i+1)}+","+entries.at(i).select("h2").at(0).text()+"\n");}
end _dsl_
 run_program(p,"build/page-parser-"+str(i)+".dslb");count=json_parse(read_file("data/article-count.txt"));articles=articles+count;pages=pages+1;
 append_file("data/web_pagination.csv","page "+str(pages)+","+str(count)+"\n");route=read_file("data/next.txt");if(is_blank(route)){break;}
}
check(pages==3&&articles==9&&is_blank(route),"pagination completion");
check(read_csv("reports/article-headlines.csv").len()==9,"article inventory");
''')

add('web_site_mirror','Mirror parsed HTML, stylesheet, script and image dependencies',r'''
mkdir("mirror/assets");r=download(base,"/site/mirror.html","data/mirror-source.html",true);check(r.status==200,"mirror seed");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let doc=html.load("data/mirror-source.html");let css=doc.select("link[rel=stylesheet]");let js=doc.select("script[src]");let images=doc.select("img[src]");let pages=doc.select("a[href]");
assert_close(float(css.len()),1.0,0.000001);assert_close(float(js.len()),1.0,0.000001);assert_close(float(images.len()),1.0,0.000001);assert_close(float(pages.len()),1.0,0.000001);
file_write("data/mirror-urls.txt",css.at(0).attr("href")+"\n"+js.at(0).attr("src")+"\n"+images.at(0).attr("src")+"\n"+pages.at(0).attr("href")+"\n");
css.at(0).set_attr("href","assets/site.css");js.at(0).set_attr("src","assets/app.js");images.at(0).set_attr("src","assets/logo.svg");pages.at(0).set_attr("href","about.html");
html.save(doc,"mirror/index.html");
end _dsl_
run_program(p,"build/mirror-parser.dslb");urls=read_lines("data/mirror-urls.txt");check(urls.len()==4,"mirror dependency count");
# The fixed list is intentionally ready to replace with a dynamic parallel_map.
results=parallel([download(base,urls[0],"mirror/assets/site.css",true),download(base,urls[1],"mirror/assets/app.js",true),download(base,urls[2],"mirror/assets/logo.svg",true),download(base,urls[3],"mirror/about.html",true)]);
bytes=0;for(result in results){check(result.status==200&&result.bytes>0,"mirror asset download");bytes=bytes+result.bytes;}
check(file_exists("mirror/index.html")&&file_exists("mirror/about.html"),"offline mirror pages");
write_file("data/web_site_mirror.csv","metric,value\ndependencies,"+str(results.len())+"\nroot pages,1\n");
write_json("reports/mirror-manifest.json",{root:"mirror/index.html",dependencies:urls,downloaded_bytes:bytes});
''',draw=r'''
scene.canvas(900,560,"#0d1726");
scene.text(45.0,55.0,26.0,"A parsed page becomes a self-contained mirror","#edf5ff");
scene.text(45.0,90.0,16.0,"Four independent dependencies downloaded with fixed parallel calls","#a9bdd5");
fn box(x:float,y:float,title:string,detail:string)->void{
 let r=scene.rect(x-98.0,y-43.0,196.0,86.0,"#17344c");r.set_attr("rx","10");
 let t=scene.text(x,y-9.0,18.0,title,"#ffffff");t.set_attr("text-anchor","middle");
 let d=scene.text(x,y+20.0,12.0,detail,"#b6e3ff");d.set_attr("text-anchor","middle");
}
box(450.0,165.0,"index.html","rewritten local references");
for(i in range(0,4)){
 let x=120.0+float(i)*220.0;let e=scene.line(450.0,218.0,x,307.0,"#79c0ff",2.5);e.set_attr("marker-end","arrow");
}
let urls=file_lines("data/mirror-urls.txt");
box(120.0,360.0,"Stylesheet",urls.at(0));box(340.0,360.0,"JavaScript",urls.at(1));
box(560.0,360.0,"SVG image",urls.at(2));box(780.0,360.0,"HTML page",urls.at(3));
scene.text(45.0,475.0,19.0,"All four responses: HTTP 200, non-empty body","#aff2ce");
scene.text(45.0,515.0,15.0,"Local output: mirror/index.html + assets/ + about.html","#a9bdd5");
scene.render_png("images/web_site_mirror.png",900,560,1);
''')
assert len(STORIES)==8
