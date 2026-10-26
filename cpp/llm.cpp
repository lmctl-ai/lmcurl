#include "core.hpp"
#include "query.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <fstream>
#include <sstream>
#include <thread>
namespace lmcurl {
namespace {
struct LlmProvider {Runtime*rt;std::string name,base,model,key_env;bool mock=false;};
struct Chat {LlmProvider*provider;std::vector<std::pair<std::string,std::string>>messages;double temperature=0;int64_t max_tokens=64,seed=0;bool json_mode=false,stream=false;std::string cassette;bool replay=false;Json tools=Json::array();};
struct LlmResult {Runtime*rt;std::string text,model,finish;int64_t input=0,output=0;double cost=0,latency=0;int64_t status=200;Json tool_calls=Json::array();};
struct Sample {Runtime*rt;std::vector<LlmResult*>values;};
struct Matrix {Runtime*rt;std::vector<LlmResult*>values;};
std::string sval(const Value&v){return v.s;}int64_t ival(const Value&v){return v.i;}double fval(const Value&v){return v.f;}bool bval(const Value&v){return v.b;}
template<class T>T&obj(const Value&v,const char*){return *static_cast<T*>(v.obj);}
bool live_enabled(){auto*v=std::getenv("LMCURL_LIVE");return v&&std::string(v)=="1";}
struct Url {std::string scheme,host,path;};
Url url_parts(const std::string&base){transport_init();
    std::unique_ptr<CURLU,decltype(&curl_url_cleanup)>u(curl_url(),curl_url_cleanup);if(!u||curl_url_set(u.get(),CURLUPART_URL,base.c_str(),0)!=CURLUE_OK)throw dslvm::Error("invalid LLM API base URL");
    auto get=[&](CURLUPart p){char*s=nullptr;auto rc=curl_url_get(u.get(),p,&s,0);std::string out=rc==CURLUE_OK?s:"";curl_free(s);return out;};
    if(!get(CURLUPART_USER).empty()||!get(CURLUPART_PASSWORD).empty()||!get(CURLUPART_QUERY).empty()||!get(CURLUPART_FRAGMENT).empty())throw dslvm::Error("LLM base URL must not contain credentials, query or fragment");
    return {get(CURLUPART_SCHEME),lower(get(CURLUPART_HOST)),get(CURLUPART_PATH)};
}
bool local_base(const std::string&base){auto u=url_parts(base);return u.host=="localhost"||u.host=="127.0.0.1"||u.host=="[::1]"||u.host=="::1";}
bool safe_provider_base(const std::string&name,const std::string&base){try{auto u=url_parts(base);if(local_base(base))return u.scheme=="http"||u.scheme=="https";if(u.scheme!="https")return false;return(name=="openrouter"&&u.host=="openrouter.ai")||(name=="deepseek"&&u.host=="api.deepseek.com")||(name=="ollama"&&(u.host=="ollama.com"||u.host=="api.ollama.com"));}catch(...){return false;}}
struct Price {double input=0,output=0;std::string basis;};
Price price(const LlmProvider&p){
    if(p.mock||local_base(p.base))return {0,0,"offline/local endpoint; no external token price"};
    if(p.name=="openrouter"&&(p.model=="openrouter/free"||(p.model.size()>5&&p.model.compare(p.model.size()-5,5,":free")==0)))return {0,0,"OpenRouter explicit free catalog variant; no paid fallbacks"};
    if(p.name=="deepseek"&&(p.model=="deepseek-flash"||p.model=="deepseek-v4-flash"))return {0.30,1.20,"2026-10-26 peak cache-miss USD per million tokens"};
    if(p.name=="deepseek"&&p.model=="deepseek-v4-pro")return {1.32,3.96,"2026-10-26 peak cache-miss USD per million tokens"};
    if(p.name=="ollama")return {0,0,"Ollama local/subscription marginal token price; subscription allocation excluded"};
    throw dslvm::Error("live model has no approved price table entry");
}
Json record_result(const LlmResult&r){return{{"text",r.text},{"model",r.model},{"finish",r.finish},{"input_tokens",r.input},{"output_tokens",r.output},{"estimated_cost_usd",r.cost},{"latency_ms",r.latency},{"status",r.status},{"tool_calls",r.tool_calls}};}
void journal(Runtime&rt,Json entry){auto*path=std::getenv("LMCURL_COST_LEDGER");if(!path||!*path)return;std::ofstream out(path,std::ios::app|std::ios::binary);if(!out)throw dslvm::Error("cannot append LMCURL_COST_LEDGER");out<<rt.redact(entry.dump())<<'\n';out.flush();if(!out)throw dslvm::Error("LMCURL_COST_LEDGER write failed");}
LlmResult run_chat(Chat&c){
    auto& p=*c.provider;auto&rt=*p.rt;if(c.max_tokens<1||c.max_tokens>128||c.messages.empty()||c.messages.size()>32||!std::isfinite(c.temperature)||c.temperature<0||c.temperature>2)throw dslvm::Error("LLM request limits: 1..32 messages, 1..128 output tokens, temperature 0..2");
    std::size_t bytes=0;Json messages=Json::array();for(const auto&m:c.messages){if(m.first!="system"&&m.first!="user"&&m.first!="assistant"&&m.first!="tool")throw dslvm::Error("unsupported message role");bytes+=m.second.size();messages.push_back({{"role",m.first},{"content",m.second}});}bytes+=c.tools.dump().size();if(bytes>16384||c.tools.size()>8)throw dslvm::Error("LLM prompt/tools size limit exceeded (16 KiB, 8 tools)");
    Json request_key={{"provider",p.name},{"model",p.model},{"messages",messages},{"temperature",c.temperature},{"seed",c.seed},{"max_tokens",c.max_tokens},{"json_mode",c.json_mode},{"tools",c.tools}};auto fingerprint=sha256(rt.redact(request_key.dump()));
    if(c.replay){auto j=parse_json(read_file(c.cassette));if(j.value("fingerprint","")!=fingerprint)throw dslvm::Error("cassette does not match provider/model/prompt/parameters");LlmResult out{&rt};out.text=j.value("text","");out.model=j.value("model",p.model);out.finish=j.value("finish",std::string("replayed"));out.input=j.value("input_tokens",int64_t(0));out.output=j.value("output_tokens",int64_t(0));out.tool_calls=j.value("tool_calls",Json::array());{std::lock_guard<std::mutex>lock(rt.cost_mutex);if(rt.llm_calls.size()>=32)throw dslvm::Error("LLM call limit is 32 per run");rt.llm_calls.push_back({{"provider",p.name},{"model",p.model},{"replayed",true},{"estimated_cost_usd",0}});}return out;}
    if(!p.mock&&(!local_base(p.base)||!p.key_env.empty())&&!live_enabled())throw dslvm::Error("live LLM calls require LMCURL_LIVE=1");auto rates=p.mock?Price{}:price(p);double input_bound=static_cast<double>(bytes+2048+512*c.messages.size());double reserve_cost=(input_bound*rates.input+c.max_tokens*rates.output)/1e6;
    for(int attempt=0;attempt<3;++attempt){
        {std::lock_guard<std::mutex>lock(rt.cost_mutex);if(rt.llm_calls.size()>=32)throw dslvm::Error("LLM call limit is 32 per run");if(rt.llm_spent+rt.llm_reserved+reserve_cost>rt.llm_cap+1e-12||rt.llm_spent+rt.llm_reserved+reserve_cost>0.25+1e-12)throw dslvm::Error("LLM estimated cost cap exceeded (LMCURL_MAX_COST_USD)");rt.llm_reserved+=reserve_cost;}
        journal(rt,{{"event","reserved"},{"provider",p.name},{"model",p.model},{"reserved_usd",reserve_cost},{"input_usd_per_million",rates.input},{"output_usd_per_million",rates.output},{"price_basis",rates.basis}});
        auto started=Clock::now();LlmResult out{&rt};out.model=p.model;bool usage=false,settled=false,saw_message=p.mock;double reported_cost=0;
        auto settle=[&](double actual,const std::string&error){std::lock_guard<std::mutex>lock(rt.cost_mutex);rt.llm_reserved=std::max(0.0,rt.llm_reserved-reserve_cost);rt.llm_spent+=actual;Json entry={{"provider",p.name},{"model",out.model},{"input_tokens",out.input},{"output_tokens",out.output},{"input_usd_per_million",rates.input},{"output_usd_per_million",rates.output},{"price_basis",rates.basis},{"reserved_usd",reserve_cost},{"estimated_cost_usd",actual},{"usage_reported",usage},{"status",out.status},{"attempt",attempt+1},{"latency_ms",out.latency}};if(!error.empty())entry["error"]=error;entry["event"]="settled";rt.llm_calls.push_back(entry);settled=true;journal(rt,entry);};
        try{
            if(p.mock){out.text=c.messages.back().second.find("ping")!=std::string::npos?"pong":c.json_mode?"{\"ok\":true,\"answer\":42}":"Hello from fixture";out.input=4;out.output=3;out.finish="stop";usage=true;}
            else{
                auto base=p.base;while(!base.empty()&&base.back()=='/')base.pop_back();auto parts=url_parts(base);bool native=p.name=="ollama"&&parts.path.find("/v1")==std::string::npos;
                std::string path=native?(parts.path.size()>=4&&parts.path.substr(parts.path.size()-4)=="/api"?"/chat":"/api/chat"):(parts.path=="/"||parts.path.empty()?(p.name=="openrouter"?"/api/v1/chat/completions":"/chat/completions"):"/chat/completions");
                Session session(&rt,base);Request req(&session,"POST",path);req.llm_budgeted=true;req.follow=false;req.timeout_ms=30000;req.max_bytes=1024*1024;req.headers["content-type"]="application/json";
                Json payload={{"model",p.model},{"messages",messages},{"stream",c.stream}};
                if(native){payload["options"]={{"temperature",c.temperature},{"num_predict",c.max_tokens},{"seed",c.seed}};if(c.json_mode)payload["format"]="json";}
                else{payload["temperature"]=c.temperature;payload["max_tokens"]=c.max_tokens;payload["seed"]=c.seed;if(c.json_mode)payload["response_format"]={{"type","json_object"}};if(c.stream)payload["stream_options"]={{"include_usage",true}};if(p.name=="deepseek")payload["thinking"]={{"type","disabled"}};}
                if(!c.tools.empty())payload["tools"]=c.tools;req.body=payload.dump();
                if(p.name!="mock"&&live_enabled()&&!p.key_env.empty()){auto key=rt.env_secret(p.key_env);if(key.empty()&&!local_base(p.base))throw dslvm::Error("LLM credential environment variable is absent");if(!key.empty())req.headers["authorization"]="Bearer "+key;}
                auto response=req.send();out.status=response.status;
                if(response.status==429){out.latency=response.total_ms;settle(reserve_cost,"HTTP 429");if(attempt==2)throw dslvm::Error("LLM provider rate limited (HTTP 429)");std::this_thread::sleep_for(std::chrono::milliseconds(1000*(attempt+1)));continue;}
                if(!response.error.empty())throw dslvm::Error("LLM transport failed: "+response.error);if(out.status<200||out.status>=300)throw dslvm::Error("LLM provider returned HTTP "+std::to_string(out.status));
                auto consume=[&](const Json&j){if(j.contains("error"))throw dslvm::Error("LLM provider reported a completion error");out.model=j.value("model",out.model);if(j.contains("choices")&&!j["choices"].empty()){saw_message=true;auto v=j["choices"][0];auto m=v.value(c.stream?"delta":"message",Json::object());if(m.contains("content")&&m["content"].is_string())out.text+=m["content"].get<std::string>();if(m.contains("tool_calls"))out.tool_calls=m["tool_calls"];if(v.contains("finish_reason")&&v["finish_reason"].is_string())out.finish=v["finish_reason"].get<std::string>();}else if(j.contains("message")){saw_message=true;auto m=j["message"];out.text+=m.value("content",std::string());if(m.contains("tool_calls"))out.tool_calls=m["tool_calls"];}
                    if(j.contains("usage")){auto u=j["usage"];if(u.contains("cost")&&u["cost"].is_number())reported_cost=std::max(reported_cost,u["cost"].get<double>());if(u.contains("prompt_tokens")&&u.contains("completion_tokens")){out.input=u["prompt_tokens"].get<int64_t>();out.output=u["completion_tokens"].get<int64_t>();usage=true;}}
                    if(j.contains("prompt_eval_count")&&j.contains("eval_count")){out.input=j["prompt_eval_count"].get<int64_t>();out.output=j["eval_count"].get<int64_t>();usage=true;}if(j.value("done",false))out.finish=j.value("done_reason",std::string("stop"));};
                if(c.stream){std::istringstream lines(response.body);std::string line;while(std::getline(lines,line)){line=trim(line);if(line.empty()||line=="data: [DONE]"||line[0]==':'||line.rfind("event:",0)==0)continue;if(line.rfind("data:",0)==0)line=trim(line.substr(5));consume(parse_json(line));}}
                else consume(parse_json(response.body));
            }
            if(!saw_message)throw dslvm::Error("LLM completion contains no message");if(out.input<0||out.output<0)throw dslvm::Error("invalid token usage");out.latency=std::chrono::duration<double,std::milli>(Clock::now()-started).count();double actual=std::max(reported_cost,usage?(out.input*rates.input+out.output*rates.output)/1e6:reserve_cost);
            if(actual>reserve_cost+1e-12){settle(actual,"provider usage exceeded reservation");throw dslvm::Error("provider token usage exceeded conservative reservation; run stopped");}out.cost=actual;settle(actual,"");
            if(!c.cassette.empty()){auto cassette=record_result(out);cassette["fingerprint"]=fingerprint;cassette["provider"]=p.name;rt.write(c.cassette,cassette.dump(2)+"\n");}return out;
        }catch(...){if(!settled){out.latency=std::chrono::duration<double,std::milli>(Clock::now()-started).count();settle(reserve_cost,"request failed; reservation retained");}throw;}
    }throw dslvm::Error("LLM retry limit");
}
}
void register_llm(dslvm::Registry& r, Runtime& rt){
  r.context("llm","Llm");
  method(r,rt,"Llm","mock",{"string"},"Provider",[&rt](void*,Args&a){auto* p=rt.hold<LlmProvider>(&rt,"mock","mock",sval(a[0]),""); p->mock=true; return Value::object("Provider",p);});
  method(r,rt,"Llm","provider",{"string","string","string"},"Provider",[&rt](void*,Args&a){std::string n=sval(a[0]);std::string base=sval(a[1]);if(!safe_provider_base(n,base))throw dslvm::Error("LLM provider base must be HTTPS for the recognized provider (or localhost mock)");std::string env=local_base(base)?"":n=="openrouter"?"OPENROUTER_API_KEY":n=="deepseek"?"DEEPSEEK_API_KEY":n=="ollama"?"OLLAMA_API_KEY":"";auto* p=rt.hold<LlmProvider>(&rt,n,base,sval(a[2]),env);p->mock=false;return Value::object("Provider",p);});
  method(r,rt,"Provider","model",{"string"},"Provider",[](void*p,Args&a){static_cast<LlmProvider*>(p)->model=sval(a[0]);return Value::object("Provider",p);});
  method(r,rt,"Provider","matrix",{"string","string"},"Matrix",[&rt](void*p,Args&a){auto& base=*static_cast<LlmProvider*>(p);auto* out=rt.hold<Matrix>(&rt);std::stringstream ss(sval(a[0]));std::string model;int n=0;while(std::getline(ss,model,',')){if(model.empty()||++n>8)throw dslvm::Error("model matrix requires 1..8 models");LlmProvider copy=base;copy.model=trim(model);Chat c{&copy,{{"user",sval(a[1])}}};out->values.push_back(rt.hold<LlmResult>(run_chat(c)));}return Value::object("Matrix",out);});
  method(r,rt,"Provider","chat",{"string"},"Chat",[&rt](void*p,Args&a){return Value::object("Chat",rt.hold<Chat>(static_cast<LlmProvider*>(p),std::vector<std::pair<std::string,std::string>>{{"user",sval(a[0])}}));});
  method(r,rt,"Chat","system",{"string"},"Chat",[](void*p,Args&a){obj<Chat>(Value::object("Chat",p),"Chat").messages.insert(obj<Chat>(Value::object("Chat",p),"Chat").messages.begin(),{"system",sval(a[0])});return Value::object("Chat",p);});
  method(r,rt,"Chat","user",{"string"},"Chat",[](void*p,Args&a){obj<Chat>(Value::object("Chat",p),"Chat").messages.push_back({"user",sval(a[0])});return Value::object("Chat",p);});
  method(r,rt,"Chat","temperature",{"float"},"Chat",[](void*p,Args&a){obj<Chat>(Value::object("Chat",p),"Chat").temperature=fval(a[0]);return Value::object("Chat",p);});
  method(r,rt,"Chat","max_tokens",{"int"},"Chat",[](void*p,Args&a){obj<Chat>(Value::object("Chat",p),"Chat").max_tokens=ival(a[0]);return Value::object("Chat",p);});
  method(r,rt,"Chat","seed",{"int"},"Chat",[](void*p,Args&a){obj<Chat>(Value::object("Chat",p),"Chat").seed=ival(a[0]);return Value::object("Chat",p);});
  method(r,rt,"Chat","stream",{"bool"},"Chat",[](void*p,Args&a){obj<Chat>(Value::object("Chat",p),"Chat").stream=bval(a[0]);return Value::object("Chat",p);});
  method(r,rt,"Chat","tool",{"string","string"},"Chat",[](void*p,Args&a){auto&c=obj<Chat>(Value::object("Chat",p),"Chat");Json schema=Json::parse(sval(a[1]));c.tools.push_back({{"type","function"},{"function",{{"name",sval(a[0])},{"parameters",schema}}}});return Value::object("Chat",p);});
  method(r,rt,"Chat","json_mode",{"bool"},"Chat",[](void*p,Args&a){obj<Chat>(Value::object("Chat",p),"Chat").json_mode=bval(a[0]);return Value::object("Chat",p);});
  method(r,rt,"Chat","cassette",{"string"},"Chat",[](void*p,Args&a){auto&c=obj<Chat>(Value::object("Chat",p),"Chat");c.cassette=sval(a[0]);c.replay=false;return Value::object("Chat",p);});
  method(r,rt,"Chat","replay",{"string"},"Chat",[](void*p,Args&a){auto&c=obj<Chat>(Value::object("Chat",p),"Chat");c.cassette=sval(a[0]);c.replay=true;return Value::object("Chat",p);});
  method(r,rt,"Chat","send",{},"Result",[](void*p,Args&){return Value::object("Result",obj<Chat>(Value::object("Chat",p),"Chat").provider->rt->hold<LlmResult>(run_chat(obj<Chat>(Value::object("Chat",p),"Chat"))));});
  method(r,rt,"Result","status",{},"int",[](void*p,Args&){return Value::integer(static_cast<LlmResult*>(p)->status);});
  method(r,rt,"Result","finish",{},"string",[](void*p,Args&){return Value::string(static_cast<LlmResult*>(p)->finish);});
  method(r,rt,"Result","text",{},"string",[](void*p,Args&){return Value::string(static_cast<LlmResult*>(p)->text);});
  method(r,rt,"Result","model",{},"string",[](void*p,Args&){return Value::string(static_cast<LlmResult*>(p)->model);});
  method(r,rt,"Result","input_tokens",{},"int",[](void*p,Args&){return Value::integer(static_cast<LlmResult*>(p)->input);});
  method(r,rt,"Result","output_tokens",{},"int",[](void*p,Args&){return Value::integer(static_cast<LlmResult*>(p)->output);});
  method(r,rt,"Result","estimated_cost",{},"float",[](void*p,Args&){return Value::real(static_cast<LlmResult*>(p)->cost);});
  method(r,rt,"Result","latency_ms",{},"float",[](void*p,Args&){return Value::real(static_cast<LlmResult*>(p)->latency);});
  method(r,rt,"Result","not_contains",{"string"},"bool",[](void*p,Args&a){return Value::boolean(static_cast<LlmResult*>(p)->text.find(sval(a[0]))==std::string::npos);});
  method(r,rt,"Result","equals",{"string"},"bool",[](void*p,Args&a){return Value::boolean(static_cast<LlmResult*>(p)->text==sval(a[0]));});
  method(r,rt,"Result","length",{},"int",[](void*p,Args&){return Value::integer(static_cast<LlmResult*>(p)->text.size());});
  method(r,rt,"Result","contains",{"string"},"bool",[](void*p,Args&a){return Value::boolean(static_cast<LlmResult*>(p)->text.find(sval(a[0]))!=std::string::npos);});
  method(r,rt,"Result","matches",{"string"},"bool",[](void*p,Args&a){return Value::boolean(std::regex_search(static_cast<LlmResult*>(p)->text,std::regex(sval(a[0]))));});
  method(r,rt,"Result","json_valid",{},"bool",[](void*p,Args&){try{auto parsed=Json::parse(static_cast<LlmResult*>(p)->text);(void)parsed;return Value::boolean(true);}catch(...){return Value::boolean(false);}});
  method(r,rt,"Chat","sample",{"int","float"},"Sample",[&rt](void*p,Args&a){int64_t n=ival(a[0]);double threshold=fval(a[1]);if(n<1||n>10||threshold<0||threshold>1)throw dslvm::Error("sampling requires n 1..10 and pass rate 0..1");auto& c=obj<Chat>(Value::object("Chat",p),"Chat");auto* out=rt.hold<Sample>(&rt);for(int64_t i=0;i<n;i++)out->values.push_back(rt.hold<LlmResult>(run_chat(c)));int64_t pass=0;for(auto*v:out->values)if(!v->text.empty())pass++;if(double(pass)/n<threshold)throw dslvm::Error("LLM sample pass rate below threshold");return Value::object("Sample",out);});
  method(r,rt,"Matrix","len",{},"int",[](void*p,Args&){return Value::integer(static_cast<Matrix*>(p)->values.size());});
  method(r,rt,"Matrix","at",{"int"},"Result",[](void*p,Args&a){auto&m=*static_cast<Matrix*>(p);if(a[0].i<0||a[0].i>=static_cast<int64_t>(m.values.size()))throw dslvm::Error("matrix index out of range");return Value::object("Result",m.values[a[0].i]);});
  method(r,rt,"Sample","len",{},"int",[](void*p,Args&){return Value::integer(static_cast<Sample*>(p)->values.size());});
  method(r,rt,"Sample","at",{"int"},"Result",[](void*p,Args&a){auto&s=*static_cast<Sample*>(p);if(a[0].i<0||a[0].i>=static_cast<int64_t>(s.values.size()))throw dslvm::Error("sample index out of range");return Value::object("Result",s.values[a[0].i]);});
  r.function("llm_cost",{},"float",[&rt](Args&){std::lock_guard<std::mutex>g(rt.cost_mutex);return Value::real(rt.llm_spent);});
  r.function("llm_calls",{},"int",[&rt](Args&){std::lock_guard<std::mutex>g(rt.cost_mutex);return Value::integer(rt.llm_calls.size());});
  for(const char* name:{"openrouter","deepseek","ollama"}) method(r,rt,"Llm",name,{"string"},"Provider",[&rt,name](void*,Args&a){std::string n=name,base,key;if(n=="openrouter"){base="https://openrouter.ai/api/v1";key="OPENROUTER_API_KEY";}else if(n=="deepseek"){base="https://api.deepseek.com";key="DEEPSEEK_API_KEY";}else{auto*value=std::getenv("OLLAMA_API_BASE");if(!value||!*value)value=std::getenv("OLLAMA_HOST");base=value&&*value?value:"http://127.0.0.1:11434";if(base.find("://")==std::string::npos)base="http://"+base;key="OLLAMA_API_KEY";}if(!safe_provider_base(n,base))throw dslvm::Error("configured LLM API base is not HTTPS or loopback");return Value::object("Provider",rt.hold<LlmProvider>(&rt,n,base,a[0].s,key));});
  method(r,rt,"Llm","ledger",{"string"},"void",[&rt](void*,Args&a){Json j;{std::lock_guard<std::mutex>lock(rt.cost_mutex);j={{"schema","lmcurl-cost-v1"},{"cap_usd",rt.llm_cap},{"hard_ceiling_usd",0.25},{"estimated_cost_usd",rt.llm_spent},{"call_limit",32},{"calls",rt.llm_calls},{"price_date","2026-10-26"},{"price_table",{{"deepseek-flash",{{"input_usd_per_million",0.30},{"output_usd_per_million",1.20}}},{"deepseek-v4-pro",{{"input_usd_per_million",1.32},{"output_usd_per_million",3.96}}},{"openrouter-free",{{"input_usd_per_million",0},{"output_usd_per_million",0}}},{"ollama",{{"input_usd_per_million",0},{"output_usd_per_million",0},{"basis","local or subscription marginal rate"}}}}},{"price_sources",{"https://api-docs.deepseek.com/quick_start/pricing/","https://openrouter.ai/docs/guides/routing/model-variants/free","https://docs.ollama.com/cloud"}},{"price_notes","DeepSeek peak/cache-miss USD per million; OpenRouter explicit free variants; Ollama local/subscription marginal price excludes subscription allocation"}};}rt.write(a[0].s,j.dump(2)+"\n");return Value::none();});
  method(r,rt,"Result","json",{},"string",[](void*p,Args&){auto&r=*static_cast<LlmResult*>(p);return Value::string(r.rt->redact(record_result(r).dump()));});
  method(r,rt,"Result","tools",{},"string",[](void*p,Args&){return Value::string(static_cast<LlmResult*>(p)->tool_calls.dump());});
  method(r,rt,"Result","schema_valid",{"string"},"bool",[](void*p,Args&a){try{return Value::boolean(schema_valid(parse_json(static_cast<LlmResult*>(p)->text),a[0].s));}catch(...){return Value::boolean(false);}});
  for(const char*type:{"Sample","Matrix"}){
    method(r,rt,type,"csv",{"string"},"void",[type](void*p,Args&a){auto&v=std::string(type)=="Sample"?static_cast<Sample*>(p)->values:static_cast<Matrix*>(p)->values;Runtime*rt=std::string(type)=="Sample"?static_cast<Sample*>(p)->rt:static_cast<Matrix*>(p)->rt;std::string text="model,input_tokens,output_tokens,cost_usd,latency_ms\n";for(auto*r:v)text+=csv_cell(r->model)+","+std::to_string(r->input)+","+std::to_string(r->output)+","+std::to_string(r->cost)+","+std::to_string(r->latency)+"\n";rt->write(a[0].s,text);return Value::none();});
    method(r,rt,type,"pass_rate",{"string"},"float",[type](void*p,Args&a){auto&v=std::string(type)=="Sample"?static_cast<Sample*>(p)->values:static_cast<Matrix*>(p)->values;if(a[0].s.size()>256)throw dslvm::Error("assertion regex limit");std::regex pattern(a[0].s);int passed=0;for(auto*r:v){if(r->text.size()>8192)throw dslvm::Error("assertion text limit");if(std::regex_search(r->text,pattern))++passed;}return Value::real(v.empty()?0:double(passed)/v.size());});
  }
  method(r,rt,"Chat","assistant",{"string"},"Chat",[](void*p,Args&a){static_cast<Chat*>(p)->messages.push_back({"assistant",a[0].s});return Value::object("Chat",p);});

}
}
