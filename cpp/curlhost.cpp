// poc-curl host (R12, R22): a curl-like program driven by a DSL chunk. It starts, builds its registry (Request builder,
// Response, Mirror crawler, Output, env/has_env/json_escape), loads the bytecode, runs the chunk once with `out` as context,
// then continues: it flushes output and exits with a clear code (0 ok, 1 load error, 3 DSL run-time error, 4 out.fail()).
#include "dslvm.hpp"
#include "http.hpp"
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <thread>

namespace {

struct Request { std::string method = "GET", url, body; std::vector<std::pair<std::string, std::string>> headers; http::Options opt; };
struct Response { http::Response r; };
struct Mirror { std::string start, outDir = "mirror"; int depth = 1, maxPages = 100, delayMs = 0; size_t maxBytes = 4u * 1024u * 1024u; bool sameHost = true; std::vector<std::string> allow; };
struct MirrorResult { int pages = 0, skipped = 0, failed = 0; int64_t bytes = 0; };
struct Output { std::vector<std::string> lines; std::vector<std::string> failures; };  // out.print lines and out.fail messages, reported once at the end
struct Arena { std::vector<std::unique_ptr<Request>> requests; std::vector<std::unique_ptr<Response>> responses; std::vector<std::unique_ptr<Mirror>> mirrors; std::vector<std::unique_ptr<MirrorResult>> results; };

// ---- minimal JSON: path lookup "choices.0.message.content" over a parsed tree (strings, numbers, bools, null, arrays, objects) ----
struct Json { enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL; bool b = false; std::string s; std::vector<Json> arr; std::vector<std::pair<std::string, Json>> obj; };
struct JsonParser {
  const std::string& t; size_t i = 0;
  explicit JsonParser(const std::string& text) : t(text) {}
  [[noreturn]] void fail(const std::string& m) { throw dslvm::Error("json: " + m + " at offset " + std::to_string(i)); }
  void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i; }
  Json parse() { ws(); Json v = value(); ws(); if (i != t.size()) fail("trailing characters"); return v; }
  Json value() {
    ws();
    if (i >= t.size()) fail("unexpected end");
    char c = t[i]; Json v;
    if (c == '{') { ++i; v.kind = Json::OBJ; ws(); if (t[i] == '}') { ++i; return v; } while (true) { ws(); std::string k = str(); ws(); if (t[i] != ':') fail("expected ':'"); ++i; v.obj.emplace_back(k, value()); ws(); if (t[i] == ',') { ++i; continue; } if (t[i] == '}') { ++i; return v; } fail("expected ',' or '}'"); } }
    if (c == '[') { ++i; v.kind = Json::ARR; ws(); if (t[i] == ']') { ++i; return v; } while (true) { v.arr.push_back(value()); ws(); if (t[i] == ',') { ++i; continue; } if (t[i] == ']') { ++i; return v; } fail("expected ',' or ']'"); } }
    if (c == '"') { v.kind = Json::STR; v.s = str(); return v; }
    if (t.compare(i, 4, "true") == 0) { i += 4; v.kind = Json::BOOL; v.b = true; return v; }
    if (t.compare(i, 5, "false") == 0) { i += 5; v.kind = Json::BOOL; return v; }
    if (t.compare(i, 4, "null") == 0) { i += 4; return v; }
    size_t j = i; while (j < t.size() && std::strchr("+-0123456789.eE", t[j])) ++j;
    if (j == i) fail("unexpected character");
    v.kind = Json::NUM; v.s = t.substr(i, j - i); i = j; return v;
  }
  std::string str() {
    if (t[i] != '"') fail("expected a string");
    ++i; std::string out;
    while (i < t.size() && t[i] != '"') {
      if (t[i] == '\\') { ++i; char e = t[i++]; switch (e) { case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break; case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
        case 'u': { unsigned cp = std::stoul(t.substr(i, 4), nullptr, 16); i += 4; if (cp >= 0xD800 && cp <= 0xDBFF && t.compare(i, 2, "\\u") == 0) { unsigned lo = std::stoul(t.substr(i + 2, 4), nullptr, 16); i += 6; cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); }
          if (cp < 0x80) out += char(cp); else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); } else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); } else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); } break; }
        default: fail("bad escape"); } }
      else out += t[i++];
    }
    if (i >= t.size()) fail("unterminated string");
    ++i; return out;
  }
};
const Json* lookup(const Json& root, const std::string& path, std::string& why) {
  const Json* cur = &root; size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find('.', i); std::string key = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (cur->kind == Json::ARR) { if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos) { why = "'" + key + "' is not an array index"; return nullptr; } size_t n = std::stoul(key); if (n >= cur->arr.size()) { why = "index " + key + " out of range (" + std::to_string(cur->arr.size()) + " elements)"; return nullptr; } cur = &cur->arr[n]; }
    else if (cur->kind == Json::OBJ) { const Json* next = nullptr; for (auto& kv : cur->obj) if (kv.first == key) { next = &kv.second; break; } if (!next) { why = "no key '" + key + "'"; return nullptr; } cur = next; }
    else { why = "'" + key + "' reached a scalar"; return nullptr; }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return cur;
}
std::string json_escape(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) { switch (c) { case '"': out += "\\\""; break; case '\\': out += "\\\\"; break; case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break; default: if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; } else out += char(c); } }
  return out + "\"";
}

// ---- mirror: breadth-first crawl of same-host links, files written under outDir/host/path (never outside outDir) ----
std::vector<std::string> extract_links(const std::string& html) {
  std::vector<std::string> out; std::string lower = http::lower(html);
  for (const char* attr : {"href=", "src="}) {
    size_t p = 0;
    while ((p = lower.find(attr, p)) != std::string::npos) {
      p += std::strlen(attr); if (p >= html.size()) break;
      char q = html[p]; std::string v;
      if (q == '"' || q == '\'') { size_t e = html.find(q, p + 1); if (e == std::string::npos) break; v = html.substr(p + 1, e - p - 1); p = e + 1; }
      else { size_t e = html.find_first_of(" >\t\r\n", p); v = html.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? html.size() : e; }
      if (!v.empty() && v.rfind("mailto:", 0) != 0 && v.rfind("javascript:", 0) != 0 && v.rfind("data:", 0) != 0) out.push_back(v);
    }
  }
  return out;
}
// URL -> local relative path: host/segments, index.html for directories, query dropped, unsafe characters replaced, no '..'.
std::string local_path(const http::Url& u) {
  std::string path = u.path; size_t q = path.find('?'); if (q != std::string::npos) path.erase(q);
  std::string out = u.host; if (u.port != 80 && u.port != 443) out += "_" + std::to_string(u.port);
  size_t i = 1;
  while (i <= path.size()) {
    size_t j = path.find('/', i); std::string seg = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    std::string safe; for (unsigned char c : seg) safe += (std::isalnum(c) || c == '.' || c == '-' || c == '_') ? char(c) : '_';
    if (safe == ".." || safe == ".") safe = "_";
    if (!safe.empty()) out += "/" + safe;
    if (j == std::string::npos) { if (seg.empty()) out += "/index.html"; break; }
    i = j + 1;
  }
  if (out.rfind('/') == std::string::npos || out.substr(out.rfind('/') + 1).find('.') == std::string::npos) out += "/index.html";
  return out;
}
void mkdirs(const std::string& path) { size_t p = 0; while ((p = path.find('/', p + 1)) != std::string::npos) ::mkdir(path.substr(0, p).c_str(), 0755); }

MirrorResult run_mirror(const Mirror& m, Output&, std::ostream& log) {
  MirrorResult res;
  if (m.start.empty()) throw dslvm::Error("mirror: start(url) was not set");
  http::Url startUrl = http::parse_url(m.start);
  std::set<std::string> seen; std::deque<std::pair<std::string, int>> queue; queue.emplace_back(m.start, 0); seen.insert(m.start);
  http::Options opt; opt.maxBytes = m.maxBytes;
  while (!queue.empty() && res.pages < m.maxPages) {
    auto [url, depth] = queue.front(); queue.pop_front();
    http::Url u;
    try { u = http::parse_url(url); } catch (const http::Error& e) { log << "skip " << url << ": " << e.what() << "\n"; res.skipped++; continue; }
    if (m.sameHost && (u.host != startUrl.host || u.port != startUrl.port)) { log << "skip " << url << ": other host\n"; res.skipped++; continue; }
    http::Response r;
    try { r = http::fetch(url, "GET", {}, "", opt); } catch (const http::Error& e) { log << "fail " << url << ": " << e.what() << "\n"; res.failed++; continue; }
    if (r.status != 200) { log << "fail " << url << ": status " << r.status << "\n"; res.failed++; continue; }
    std::string type = http::lower(r.header("Content-Type"));
    bool allowed = m.allow.empty(); for (auto& a : m.allow) if (type.rfind(http::lower(a), 0) == 0) allowed = true;
    if (!allowed) { log << "skip " << url << ": content-type " << type << "\n"; res.skipped++; continue; }
    std::string rel = local_path(http::parse_url(r.finalUrl)), full = m.outDir + "/" + rel;
    mkdirs(full); std::ofstream out(full, std::ios::binary);
    if (!out) throw dslvm::Error("mirror: cannot write " + full);
    out << r.body; res.pages++; res.bytes += long(r.body.size()); log << "saved " << url << " -> " << rel << " (" << r.body.size() << " bytes)\n";
    if (depth < m.depth && type.rfind("text/html", 0) == 0) for (auto& link : extract_links(r.body)) {
      std::string abs; try { abs = http::resolve(r.finalUrl, link); } catch (...) { continue; }
      size_t hash = abs.find('#'); if (hash != std::string::npos) abs.erase(hash);
      if (seen.insert(abs).second) queue.emplace_back(abs, depth + 1);
    }
    if (m.delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(m.delayMs));
  }
  return res;
}

// (1) The registry: the implementation of schema/curl.dsl.json, declared with the schema's types so `--dsl-signatures`
// can be diffed against `lmscript --dsl-signatures schema/curl.dsl.json` (spec v4 S0, A02). The types are declarations
// only: the compiler checked every call, the VM trusts the bytecode (R33/R34).
void register_schema(dslvm::Registry& reg, Arena& arena, Output& output) {
  reg.context("out", "Output");
  auto req = [](void* p) { return static_cast<Request*>(p); }; auto self = [](void* p) { return dslvm::Value::object("Request", p); };
  reg.function("request", {}, "Request", [&](dslvm::Args&) { arena.requests.push_back(std::make_unique<Request>()); return dslvm::Value::object("Request", arena.requests.back().get()); });
  reg.method("Request", "method", {"string"}, "Request", [&](void* p, dslvm::Args& a) { std::string m = a[0].s; for (auto& c : m) c = char(std::toupper(static_cast<unsigned char>(c))); if (m != "GET" && m != "POST" && m != "PUT" && m != "PATCH" && m != "DELETE" && m != "HEAD") throw dslvm::Error("Request.method: unsupported method '" + a[0].s + "'"); req(p)->method = m; return self(p); });
  reg.method("Request", "url", {"string"}, "Request", [&](void* p, dslvm::Args& a) { http::parse_url(a[0].s); req(p)->url = a[0].s; return self(p); });
  reg.method("Request", "header", {"string", "string"}, "Request", [&](void* p, dslvm::Args& a) { if (a[0].s.empty() || a[0].s.find_first_of(":\r\n") != std::string::npos || a[1].s.find_first_of("\r\n") != std::string::npos) throw dslvm::Error("Request.header: invalid header name or value"); req(p)->headers.emplace_back(a[0].s, a[1].s); return self(p); });
  reg.method("Request", "body", {"string"}, "Request", [&](void* p, dslvm::Args& a) { req(p)->body = a[0].s; return self(p); });
  reg.method("Request", "timeout_ms", {"int"}, "Request", [&](void* p, dslvm::Args& a) { if (a[0].i <= 0) throw dslvm::Error("Request.timeout_ms must be positive"); req(p)->opt.timeoutMs = a[0].i; return self(p); });
  reg.method("Request", "max_bytes", {"int"}, "Request", [&](void* p, dslvm::Args& a) { if (a[0].i <= 0) throw dslvm::Error("Request.max_bytes must be positive"); req(p)->opt.maxBytes = size_t(a[0].i); return self(p); });
  reg.method("Request", "max_redirects", {"int"}, "Request", [&](void* p, dslvm::Args& a) { if (a[0].i < 0) throw dslvm::Error("Request.max_redirects must not be negative"); req(p)->opt.maxRedirects = a[0].i; return self(p); });
  reg.method("Request", "send", {}, "Response", [&](void* p, dslvm::Args&) {
    Request& r = *req(p); if (r.url.empty()) throw dslvm::Error("Request.send: url(...) was not set");
    arena.responses.push_back(std::make_unique<Response>());
    try { arena.responses.back()->r = http::fetch(r.url, r.method, r.headers, r.body, r.opt); } catch (const http::Error& e) { throw dslvm::Error("Request.send " + r.method + " " + r.url + ": " + e.what()); }
    return dslvm::Value::object("Response", arena.responses.back().get());
  });
  auto resp = [](void* p) { return &static_cast<Response*>(p)->r; };
  reg.method("Response", "status", {}, "int", [&](void* p, dslvm::Args&) { return dslvm::Value::integer(resp(p)->status); });
  reg.method("Response", "ok", {}, "bool", [&](void* p, dslvm::Args&) { return dslvm::Value::boolean(resp(p)->status >= 200 && resp(p)->status < 300); });
  reg.method("Response", "body", {}, "string", [&](void* p, dslvm::Args&) { return dslvm::Value::string(resp(p)->body); });
  reg.method("Response", "header", {"string"}, "string", [&](void* p, dslvm::Args& a) { return dslvm::Value::string(resp(p)->header(a[0].s)); });
  auto jsonAt = [&](void* p, const std::string& path, std::string& why) -> const Json* { static std::map<void*, Json> cache; auto it = cache.find(p); if (it == cache.end()) { try { it = cache.emplace(p, JsonParser(resp(p)->body).parse()).first; } catch (const dslvm::Error& e) { why = e.what(); return nullptr; } } return lookup(it->second, path, why); };
  reg.method("Response", "has_json", {"string"}, "bool", [&](void* p, dslvm::Args& a) { std::string why; return dslvm::Value::boolean(jsonAt(p, a[0].s, why) != nullptr); });
  reg.method("Response", "json_text", {"string"}, "string", [&](void* p, dslvm::Args& a) {
    std::string why; const Json* v = jsonAt(p, a[0].s, why); if (!v) throw dslvm::Error("Response.json_text(\"" + a[0].s + "\"): " + why);
    if (v->kind == Json::STR || v->kind == Json::NUM) return dslvm::Value::string(v->s);
    if (v->kind == Json::BOOL) return dslvm::Value::string(v->b ? "true" : "false");
    if (v->kind == Json::NUL) return dslvm::Value::string("null");
    throw dslvm::Error("Response.json_text(\"" + a[0].s + "\"): the value is " + (v->kind == Json::ARR ? "an array" : "an object") + ", not a scalar");
  });
  auto mir = [](void* p) { return static_cast<Mirror*>(p); }; auto mself = [](void* p) { return dslvm::Value::object("Mirror", p); };
  reg.function("mirror", {}, "Mirror", [&](dslvm::Args&) { arena.mirrors.push_back(std::make_unique<Mirror>()); return dslvm::Value::object("Mirror", arena.mirrors.back().get()); });
  reg.method("Mirror", "start", {"string"}, "Mirror", [&](void* p, dslvm::Args& a) { http::parse_url(a[0].s); mir(p)->start = a[0].s; return mself(p); });
  reg.method("Mirror", "depth", {"int"}, "Mirror", [&](void* p, dslvm::Args& a) { if (a[0].i < 0 || a[0].i > 10) throw dslvm::Error("Mirror.depth must be 0..10"); mir(p)->depth = a[0].i; return mself(p); });
  reg.method("Mirror", "same_host", {"bool"}, "Mirror", [&](void* p, dslvm::Args& a) { mir(p)->sameHost = a[0].b; return mself(p); });
  reg.method("Mirror", "allow", {"string"}, "Mirror", [&](void* p, dslvm::Args& a) { mir(p)->allow.push_back(a[0].s); return mself(p); });
  reg.method("Mirror", "out_dir", {"string"}, "Mirror", [&](void* p, dslvm::Args& a) { if (a[0].s.empty()) throw dslvm::Error("Mirror.out_dir must not be empty"); mir(p)->outDir = a[0].s; return mself(p); });
  reg.method("Mirror", "max_pages", {"int"}, "Mirror", [&](void* p, dslvm::Args& a) { if (a[0].i <= 0) throw dslvm::Error("Mirror.max_pages must be positive"); mir(p)->maxPages = a[0].i; return mself(p); });
  reg.method("Mirror", "max_bytes", {"int"}, "Mirror", [&](void* p, dslvm::Args& a) { if (a[0].i <= 0) throw dslvm::Error("Mirror.max_bytes must be positive"); mir(p)->maxBytes = size_t(a[0].i); return mself(p); });
  reg.method("Mirror", "delay_ms", {"int"}, "Mirror", [&](void* p, dslvm::Args& a) { if (a[0].i < 0) throw dslvm::Error("Mirror.delay_ms must not be negative"); mir(p)->delayMs = a[0].i; return mself(p); });
  reg.method("Mirror", "run", {}, "MirrorResult", [&](void* p, dslvm::Args&) { arena.results.push_back(std::make_unique<MirrorResult>(run_mirror(*mir(p), output, std::cerr))); return dslvm::Value::object("MirrorResult", arena.results.back().get()); });
  auto mr = [](void* p) { return static_cast<MirrorResult*>(p); };
  reg.method("MirrorResult", "pages", {}, "int", [&](void* p, dslvm::Args&) { return dslvm::Value::integer(mr(p)->pages); });
  reg.method("MirrorResult", "skipped", {}, "int", [&](void* p, dslvm::Args&) { return dslvm::Value::integer(mr(p)->skipped); });
  reg.method("MirrorResult", "failed", {}, "int", [&](void* p, dslvm::Args&) { return dslvm::Value::integer(mr(p)->failed); });
  reg.method("MirrorResult", "bytes", {}, "int", [&](void* p, dslvm::Args&) { return dslvm::Value::integer(mr(p)->bytes); });
  reg.method("Output", "print", {"string"}, "void", [&](void* p, dslvm::Args& a) { static_cast<Output*>(p)->lines.push_back(a[0].s); return dslvm::Value::none(); });
  reg.method("Output", "write_file", {"string", "string"}, "void", [&](void*, dslvm::Args& a) { std::ofstream f(a[0].s, std::ios::binary); if (!f) throw dslvm::Error("Output.write_file: cannot write " + a[0].s); f << a[1].s; return dslvm::Value::none(); });
  reg.method("Output", "fail", {"string"}, "void", [&](void* p, dslvm::Args& a) { std::cerr << "fail: " << a[0].s << "\n"; static_cast<Output*>(p)->failures.push_back(a[0].s); return dslvm::Value::none(); });
  reg.function("env", {"string"}, "string", [&](dslvm::Args& a) { const char* v = std::getenv(a[0].s.c_str()); if (!v) throw dslvm::Error("env: " + a[0].s + " is not set in the host environment (test with has_env)"); return dslvm::Value::string(v); });
  reg.function("has_env", {"string"}, "bool", [&](dslvm::Args& a) { return dslvm::Value::boolean(std::getenv(a[0].s.c_str()) != nullptr); });
  reg.function("json_escape", {"string"}, "string", [&](dslvm::Args& a) { return dslvm::Value::string(json_escape(a[0].s)); });
  (void)output;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dslPath, schemaPath;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](const char* flag) { if (i + 1 >= argc) { std::cerr << "error: " << flag << " needs a value\n"; std::exit(2); } return std::string(argv[++i]); };
    if (a == "--dsl") dslPath = value("--dsl"); else if (a == "--schema") schemaPath = value("--schema");
    else if (a == "--dsl-signatures") { Arena pa; Output po; dslvm::Registry preg; register_schema(preg, pa, po); for (const auto& s : preg.signatures()) std::cout << s << "\n"; return 0; }
    else { std::cerr << "usage: curlhost --dsl FILE --schema FILE | --dsl-signatures\n"; return 2; }
  }
  if (dslPath.empty() || schemaPath.empty()) { std::cerr << "error: --dsl FILE and --schema FILE are required\n"; return 2; }

  Arena arena; Output output;
  dslvm::Registry reg;
  register_schema(reg, arena, output);

  // (2) Load and verify the DSL; nothing has happened yet. A refusal is one JSON line, phase load, exit 3 (spec v4 S0, A03).
  dslvm::Program program;
  try { program = dslvm::Program::load(dslvm::read_file(dslPath), dslvm::read_file(schemaPath), reg); }
  catch (const dslvm::Error& e) { std::cerr << "error: " << dslPath << ": " << e.what() << "\n"; return dslvm::report_error("load", e); }
  std::cerr << "loaded " << dslPath << " (" << program.constant_count() << " constants, " << program.code_size() << " code bytes)\n";

  // The chunk is the callback: it configures and triggers requests through the host's objects. A host or VM failure
  // during the run is one JSON line, phase run, exit 3; the diagnostic also goes to stderr for the human reader.
  dslvm::Context ctx; ctx.set("out", &output);
  try { program.run(ctx, reg); }
  catch (const dslvm::Error& e) { std::cerr << "dsl runtime error: " << e.what() << "\n"; return dslvm::report_error("run", e); }

  // (3) The host continues: the summary on stderr, then exactly one JSON result line on stdout. `out.fail` messages make
  // the run an application failure (exit 4, code APP_FAIL); otherwise the result carries every `out.print` line in order.
  std::cerr << "done: " << arena.requests.size() << " request(s), " << arena.mirrors.size() << " mirror(s), " << output.failures.size() << " failure(s)\n";
  if (!output.failures.empty()) {
    std::string all; for (const auto& f : output.failures) all += (all.empty() ? "" : "; ") + f;
    return dslvm::report_app_failure(all);
  }
  std::string lines; for (const auto& l : output.lines) lines += (lines.empty() ? "" : ", ") + dslvm::json_escape(l);
  return dslvm::report_ok("{\"output\": [" + lines + "], \"requests\": " + std::to_string(arena.requests.size()) + ", \"mirrors\": " + std::to_string(arena.mirrors.size()) + "}");
}
