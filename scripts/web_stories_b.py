"""Measured offline website audits: outer orchestration plus native lmhtml parsing."""
STORIES=[]
def add(name,title,story,**options): STORIES.append(dict(name=name,title=title,story=story,**options))

add('web_seo_audit','SEO audit: metadata, headings and image alternatives',r'''
x=download(base,"/site/seo.html","data/seo.html",true);check(x.status==200,"SEO download");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let d=html.load("data/seo.html");
let titles=d.select("title");let meta=d.select("meta[name=description]");let h1=d.select("h1");let images=d.select("img");
let missing_title=0;if(titles.len()==0){missing_title=1;}
let missing_meta=0;if(meta.len()==0){missing_meta=1;}
let extra_h1=0;if(h1.len()>1){extra_h1=h1.len()-1;}
let bad_alt=0;
for(i in range(0,images.len())){if(!images.at(i).has_attr("alt")){bad_alt=bad_alt+1;report.add("missing-alt",images.at(i).path(),0,images.at(i).attr("src"));}}
assert_close(float(titles.len()),1.0,0.000001);assert_close(float(missing_meta),1.0,0.000001);
assert_close(float(extra_h1),1.0,0.000001);assert_close(float(bad_alt),1.0,0.000001);
file_write("data/web_seo_audit.csv","metric,value\nmissing title,"+str(missing_title)+"\nmissing description,"+str(missing_meta)+"\nextra h1,"+str(extra_h1)+"\nimages missing alt,"+str(bad_alt)+"\n");
report.write_json("reports/seo-findings.json");
end _dsl_
run_program(p,"build/seo-parser.dslb");
''')

add('web_image_redirect_audit','Find broken image URLs and follow every redirect hop',r'''
x=download(base,"/site/audit.html","data/audit.html",true);check(x.status==200,"image audit download");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let d=html.load("data/audit.html");let images=d.select("img");file_write("data/image-urls.txt","");
for(i in range(0,images.len())){file_append("data/image-urls.txt",images.at(i).attr("src")+"\n");}
file_write("data/redirect-url.txt",d.select("a").at(0).attr("href"));
end _dsl_
run_program(p,"build/images-parser.dslb");
checked=0;broken=0;report="url,status\n";
for(route in read_lines("data/image-urls.txt")){
 if(is_blank(route)){continue;}
 r=download(base,route,"data/image-"+str(checked),false);checked=checked+1;
 if(r.status>=400){broken=broken+1;}report=report+route+","+str(r.status)+"\n";
}
route=read_file("data/redirect-url.txt");hops=0;terminal=0;
for(i in range(0,6)){
 r=download(base,route,"data/redirect-"+str(i),false);report=report+route+","+str(r.status)+"\n";
 if(r.status>=300&&r.status<400&&!is_blank(r.location)){hops=hops+1;route=r.location;}else{terminal=r.status;break;}
}
check(checked==2&&broken==1&&hops==2&&terminal==200,"image/redirect findings differ");
write_file("reports/image-and-redirect.csv",report);
write_file("data/web_image_redirect_audit.csv","metric,value\nimage URLs,"+str(checked)+"\nbroken images,"+str(broken)+"\nredirect hops,"+str(hops)+"\n");
''')

add('web_api_html_consistency','Compare each product SKU, price and stock with the API',r'''
# Fixed fan-out; replace this list with parallel_map when the compiler exposes it.
responses=parallel([download(base,"/site/products.html","data/products.html",true),download(base,"/site/api/products.json","data/api-products.json",true)]);
check(responses[0].status==200&&responses[1].status==200,"HTML/API downloads");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let rows=html.load("data/products.html").select("table tbody tr");
file_write("data/html-products.csv","sku,price,stock\n");
for(i in range(0,rows.len())){let cells=rows.at(i).select("td");if(cells.len()==3){file_append("data/html-products.csv",cells.at(0).text()+","+cells.at(1).text()+","+cells.at(2).text()+"\n");}}
end _dsl_
run_program(p,"build/consistency-parser.dslb");
html_rows=read_csv("data/html-products.csv");api=read_json("data/api-products.json").products;
check(html_rows.len()==3&&api.len()==html_rows.len(),"product counts");mismatches=0;report="sku,field,html,api\n";
for(i in range(0,html_rows.len())){
 h=html_rows[i];a=api[i];
 if(h.sku!=a.sku){mismatches=mismatches+1;}
 if(json_parse(str(h.price))!=a.price){mismatches=mismatches+1;}
 if(json_parse(str(h.stock))!=a.stock){mismatches=mismatches+1;}
 report=report+h.sku+",price,"+str(h.price)+","+str(a.price)+"\n";
 report=report+h.sku+",stock,"+str(h.stock)+","+str(a.stock)+"\n";
}
check(mismatches==0,"HTML/API field mismatch");write_file("reports/product-consistency.csv",report);
write_file("data/web_api_html_consistency.csv","metric,value\nproducts compared,"+str(html_rows.len())+"\nfields compared,"+str(html_rows.len()*3)+"\nfield mismatches,"+str(mismatches)+"\n");
''')

add('web_content_diff','A second crawl changes content; a third crawl stays stable',r'''
a=download(base,"/site/changes.html","data/before.html",true);
b=download(base,"/site/changes.html","data/after.html",true);
c=download(base,"/site/changes.html","data/unchanged.html",true);
check(a.status==200&&b.status==200&&c.status==200,"content crawls");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let before=html.load("data/before.html");let after=html.load("data/after.html");let stable=html.load("data/unchanged.html");
let changed=html.diff(before,after);let unchanged=html.diff(after,stable);
if(changed<=0){report.add("assertion","diff",0,"changed content not detected");report.fail_if_any();}
assert_close(float(unchanged),0.0,0.000001);
file_write("data/diff-before.txt",before.select("p").at(0).text());file_write("data/diff-after.txt",after.select("p").at(0).text());
file_write("data/diff-count.txt",str(changed));
file_write("data/web_content_diff.csv","metric,value\nchanged crawl,"+str(changed)+"\nstable crawl,"+str(unchanged)+"\n");
end _dsl_
run_program(p,"build/diff-parser.dslb");
''',draw=r'''
scene.canvas(900,560,"#0d1726");
scene.text(50.0,65.0,28.0,"Two crawls, one real content change","#edf5ff");
scene.text(50.0,103.0,16.0,"lmcurl downloads the snapshots; lmhtml compares their DOMs","#a9bdd5");
let a=scene.rect(65.0,165.0,320.0,220.0,"#17344c");a.set_attr("rx","14");
let b=scene.rect(515.0,165.0,320.0,220.0,"#174638");b.set_attr("rx","14");
scene.text(90.0,215.0,19.0,"FIRST CRAWL","#a9bdd5");
scene.text(90.0,280.0,32.0,file_read("data/diff-before.txt"),"#ffffff");
scene.text(540.0,215.0,19.0,"SECOND CRAWL","#aff2ce");
scene.text(540.0,280.0,32.0,file_read("data/diff-after.txt"),"#ffffff");
let arrow=scene.line(400.0,270.0,495.0,270.0,"#79c0ff",3.0);arrow.set_attr("marker-end","arrow");
scene.text(65.0,450.0,20.0,"Structural differences: "+file_read("data/diff-count.txt"),"#edf5ff");
scene.text(65.0,492.0,17.0,"A third fetch matches the second: zero new differences.","#a9bdd5");
scene.render_png("images/web_content_diff.png",900,560,1);
''')

add('web_accessibility_scan','Scan basic labels, image alternatives and empty controls',r'''
x=download(base,"/site/a11y.html","data/a11y.html",true);check(x.status==200,"accessibility download");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let d=html.load("data/a11y.html");let images=d.select("img");let inputs=d.select("input");let labels=d.select("label");
let missing_alt=0;let unlabeled=0;let empty_buttons=0;let empty_links=0;
for(i in range(0,images.len())){if(!images.at(i).has_attr("alt")){missing_alt=missing_alt+1;}}
for(i in range(0,inputs.len())){
 let n=inputs.at(i);let named=false;
 if(n.attr("type")=="hidden"){continue;}
 if(str_len(str_trim(n.attr("aria-label")))>0||str_len(str_trim(n.attr("title")))>0||n.parent().tag()=="label"){named=true;}
 for(j in range(0,labels.len())){if(str_len(n.attr("id"))>0&&labels.at(j).attr("for")==n.attr("id")){named=true;}}
 if(!named){unlabeled=unlabeled+1;}
}
let buttons=d.select("button");for(i in range(0,buttons.len())){let n=buttons.at(i);if(str_len(str_trim(n.text()))==0&&str_len(str_trim(n.attr("aria-label")))==0){empty_buttons=empty_buttons+1;}}
let links=d.select("a");for(i in range(0,links.len())){let n=links.at(i);if(str_len(str_trim(n.text()))==0&&str_len(str_trim(n.attr("aria-label")))==0){empty_links=empty_links+1;}}
assert_close(float(missing_alt),1.0,0.000001);assert_close(float(unlabeled),1.0,0.000001);
assert_close(float(empty_buttons),1.0,0.000001);assert_close(float(empty_links),1.0,0.000001);
file_write("data/web_accessibility_scan.csv","metric,value\nmissing alt,"+str(missing_alt)+"\nunlabeled input,"+str(unlabeled)+"\nempty button,"+str(empty_buttons)+"\nempty link,"+str(empty_links)+"\n");
end _dsl_
run_program(p,"build/a11y-parser.dslb");
''')

add('web_metadata_extractor','Extract and inspect JSON-LD and OpenGraph fields',r'''
x=download(base,"/site/metadata.html","data/metadata.html",true);check(x.status==200,"metadata download");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let d=html.load("data/metadata.html");let blocks=d.select("script[type='application/ld+json']");let og=d.select("meta[property]");
assert_close(float(blocks.len()),1.0,0.000001);
file_write("data/jsonld.json",blocks.at(0).text());file_write("data/opengraph.csv","property,content\n");
for(i in range(0,og.len())){file_append("data/opengraph.csv",og.at(i).attr("property")+","+og.at(i).attr("content")+"\n");}
file_write("data/web_metadata_extractor.csv","metric,value\nJSON-LD blocks,"+str(blocks.len())+"\nOpenGraph fields,"+str(og.len())+"\n");
end _dsl_
run_program(p,"build/metadata-parser.dslb");
ld=read_json("data/jsonld.json");og=read_csv("data/opengraph.csv");
check(ld["@type"]=="Article"&&ld.headline=="Fixture","JSON-LD values");
check(og.len()==2&&og[0].content=="Fixture metadata"&&og[1].content=="/site/assets/logo.svg","OpenGraph values");
write_json("reports/extracted-metadata.json",{json_ld:ld,open_graph:og});
''')

add('web_form_inventory','Inventory form actions, methods and named controls',r'''
x=download(base,"/site/forms.html","data/forms.html",true);check(x.status==200,"forms download");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let forms=html.load("data/forms.html").select("form");let gets=0;let posts=0;let total=0;let named=0;
file_write("reports/form-controls.csv","method,action,tag,name,type\n");
for(i in range(0,forms.len())){
 let form=forms.at(i);let method=form.attr("method");if(str_len(method)==0){method="get";}
 if(method=="get"){gets=gets+1;}if(method=="post"){posts=posts+1;}
 let controls=form.select("input,textarea,select,button");
 for(j in range(0,controls.len())){let control=controls.at(j);total=total+1;if(str_len(control.attr("name"))>0){named=named+1;}
 file_append("reports/form-controls.csv",method+","+form.attr("action")+","+control.tag()+","+control.attr("name")+","+control.attr("type")+"\n");}
}
assert_close(float(forms.len()),2.0,0.000001);assert_close(float(gets),1.0,0.000001);assert_close(float(posts),1.0,0.000001);
assert_close(float(total),5.0,0.000001);assert_close(float(named),3.0,0.000001);
file_write("data/web_form_inventory.csv","metric,value\nforms,"+str(forms.len())+"\nGET forms,"+str(gets)+"\nPOST forms,"+str(posts)+"\ncontrols,"+str(total)+"\nnamed controls,"+str(named)+"\n");
end _dsl_
run_program(p,"build/form-parser.dslb");
''')

add('web_robots_crawler','Respect robots exclusions and wait between allowed requests',r'''
r=download(base,"/site/robots.txt","data/robots.txt",true);check(r.status==200,"robots download");
disallow=[];delay_ms=0;applies=false;
for(line in read_lines("data/robots.txt")){
 line=trim(line);if(starts_with(line,"User-agent:")){applies=trim(replace(line,"User-agent:",""))=="*";}
 if(applies&&starts_with(line,"Disallow:")){rule=trim(replace(line,"Disallow:",""));if(!is_blank(rule)){push(disallow,rule);}}
 if(applies&&starts_with(line,"Crawl-delay:")){delay_ms=json_parse(trim(replace(line,"Crawl-delay:","")))*1000;}
}
check(disallow.len()==1&&delay_ms==20,"fixture robots policy");sleep_ms(delay_ms);
h=download(base,"/site/index.html","data/robots-index.html",true);check(h.status==200,"crawler seed");
p=_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let links=html.load("data/robots-index.html").links();file_write("data/robot-links.txt","");
for(i in range(0,links.len())){file_append("data/robot-links.txt",links.at(i)+"\n");}
end _dsl_
run_program(p,"build/robots-parser.dslb");
visited=[];skipped=0;ok=0;missing=0;previous=now();minimum_gap=100000;
for(route in read_lines("data/robot-links.txt")){
 if(is_blank(route)){continue;}blocked=false;
 for(rule in disallow){if(starts_with(route,rule)){blocked=true;}}
 if(blocked){skipped=skipped+1;continue;}
 sleep_ms(delay_ms);started=now();gap=started-previous;if(gap<minimum_gap){minimum_gap=gap;}
 result=download(base,route,"data/robot-page-"+str(visited.len())+".html",true);previous=now();push(visited,route);
 if(result.status==200){ok=ok+1;}else{missing=missing+1;}
}
check(skipped==1&&visited.len()==3&&ok==2&&missing==1,"robots crawl results");
check(minimum_gap>=delay_ms,"politeness interval");
for(route in visited){check(!starts_with(route,"/site/private/"),"disallowed URL visited");}
write_json("reports/robots-crawl.json",{visited:visited,skipped:skipped,minimum_gap_ms:minimum_gap,policy_delay_ms:delay_ms});
write_file("data/web_robots_crawler.csv","metric,value\nallowed fetched,"+str(visited.len())+"\nhealthy pages,"+str(ok)+"\nbroken pages,"+str(missing)+"\nblocked skipped,"+str(skipped)+"\n");
''')
assert len(STORIES)==8
