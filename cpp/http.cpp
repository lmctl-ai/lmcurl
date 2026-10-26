#include "core.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <filesystem>
#include <openssl/evp.h>
#include <thread>
namespace lmcurl {
namespace {
struct CurlGlobal {
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw dslvm::Error("libcurl initialization failed");
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};
void init() { static CurlGlobal global; }
void text(const std::string &v, std::size_t max, const char *label) {
  if (v.size() > max || v.find('\0') != std::string::npos)
    throw dslvm::Error(std::string(label) + " exceeds limits or contains NUL");
}
void header(const std::string &name, const std::string &value) {
  text(name, 128, "header name");
  text(value, 8192, "header value");
  if (name.empty() ||
      name.find_first_not_of(
          "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") !=
          std::string::npos ||
      value.find_first_of("\r\n") != std::string::npos)
    throw dslvm::Error("invalid HTTP header");
}
void mark(Runtime &rt, const std::string &name, const std::string &value) {
  auto key = lower(name);
  if (key == "authorization" || key == "proxy-authorization" ||
      key.find("api-key") != std::string::npos || key.find("api_key") != std::string::npos) {
    rt.secret(value);
    auto space = value.find(' ');
    if (space != std::string::npos)
      rt.secret(value.substr(space + 1));
  }
}
struct Transfer {
  Response result;
  std::size_t max, header_bytes = 0;
  Clock::time_point started;
  std::exception_ptr exception;
};
std::size_t body_cb(char *p, std::size_t size, std::size_t count, void *data) noexcept {
  auto &t = *static_cast<Transfer *>(data);
  try {
    if (size && count > SIZE_MAX / size)
      return 0;
    auto n = size * count;
    if (n > t.max - t.result.body.size())
      throw dslvm::Error("HTTP response size limit exceeded");
    membudget::require(n);
    if (t.result.body.empty())
      t.result.first_data_ms =
          std::chrono::duration<double, std::milli>(Clock::now() - t.started).count();
    t.result.body.append(p, n);
    return n;
  } catch (...) {
    t.exception = std::current_exception();
    return 0;
  }
}
std::size_t header_cb(char *p, std::size_t size, std::size_t count, void *data) noexcept {
  auto &t = *static_cast<Transfer *>(data);
  try {
    if (size && count > SIZE_MAX / size)
      return 0;
    auto n = size * count;
    if (n > 65536 - t.header_bytes)
      throw dslvm::Error("HTTP header size limit exceeded");
    t.header_bytes += n;
    std::string line(p, n);
    if (line.rfind("HTTP/", 0) == 0) {
      t.result.headers.clear();
      t.result.body.clear();
    }
    auto colon = line.find(':');
    if (colon != std::string::npos) {
      auto name = lower(trim(line.substr(0, colon)));
      auto value = trim(line.substr(colon + 1));
      auto &old = t.result.headers[name];
      if (!old.empty())
        old += (name == "set-cookie" ? "\n" : ",");
      old += value;
    }
    return n;
  } catch (...) {
    t.exception = std::current_exception();
    return 0;
  }
}
struct SList {
  curl_slist *p = nullptr;
  ~SList() { curl_slist_free_all(p); }
  void add(const std::string &s) {
    auto *next = curl_slist_append(p, s.c_str());
    if (!next)
      throw std::bad_alloc();
    p = next;
  }
};
std::string escape(CURL *c, const std::string &s) {
  char *p = curl_easy_escape(c, s.data(), static_cast<int>(s.size()));
  if (!p)
    throw std::bad_alloc();
  std::string out(p);
  curl_free(p);
  return out;
}
} // namespace
void transport_init() { init(); }
struct CookieState {
  CURLSH *handle;
  std::mutex mutex;
  Reservation reservation{1024 * 1024};
  CookieState() {
    init();
    handle = curl_share_init();
    if (!handle)
      throw std::bad_alloc();
    curl_share_setopt(handle, CURLSHOPT_USERDATA, this);
    curl_share_setopt(
        handle, CURLSHOPT_LOCKFUNC, +[](CURL *, curl_lock_data, curl_lock_access, void *p) {
          static_cast<CookieState *>(p)->mutex.lock();
        });
    curl_share_setopt(
        handle, CURLSHOPT_UNLOCKFUNC,
        +[](CURL *, curl_lock_data, void *p) { static_cast<CookieState *>(p)->mutex.unlock(); });
    curl_share_setopt(handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
  }
  ~CookieState() { curl_share_cleanup(handle); }
};
Session::Session(Runtime *r, std::string b)
    : runtime(r), base(std::move(b)), state(std::make_shared<CookieState>()) {
  text(base, 8192, "base URL");
}
Session::~Session() = default;
std::string Session::expand(const std::string &source) const {
  std::lock_guard<std::mutex> lock(mutex);
  std::string out = source;
  std::size_t pos = 0;
  while ((pos = out.find("{{", pos)) != std::string::npos) {
    auto end = out.find("}}", pos + 2);
    if (end == std::string::npos)
      throw dslvm::Error("unterminated session variable");
    auto key = out.substr(pos + 2, end - pos - 2);
    auto found = variables.find(key);
    if (found == variables.end())
      throw dslvm::Error("missing session variable: " + key);
    out.replace(pos, end - pos + 2, found->second);
    pos += found->second.size();
    if (out.size() > 8 * 1024 * 1024)
      throw dslvm::Error("expanded input exceeds size limit");
  }
  return out;
}
Response Session::perform(const Request &input) const {
  init();
  Request request = input;
  if (!request.literal) {
    request.url = expand(request.url);
    request.body = expand(request.body);
    std::map<std::string, std::string> combined;
    {
      std::lock_guard<std::mutex> lock(mutex);
      combined = headers;
    }
    for (const auto &kv : request.headers)
      combined[kv.first] = kv.second;
    for (auto &kv : combined)
      kv.second = expand(kv.second);
    request.headers = std::move(combined);
    std::map<std::string, std::string> form;
    for (const auto &kv : request.form)
      form[expand(kv.first)] = expand(kv.second);
    request.form = std::move(form);
    for (auto &part : request.parts)
      if (!part.file)
        part.value = expand(part.value);
    request.literal = true;
    request.deadline = Clock::now() + std::chrono::milliseconds(request.timeout_ms);
  }
  if (request.timeout_ms < 1 || request.timeout_ms > 120000 || request.retries < 0 ||
      request.retries > 5 || request.retry_ms < 0 || request.retry_ms > 2000 ||
      request.max_bytes < 1 || request.max_bytes > 8 * 1024 * 1024)
    throw dslvm::Error("request timeout/retry/size limits invalid");
  auto target = request.url;
  if (target.rfind("http://", 0) != 0 && target.rfind("https://", 0) != 0)
    target = base + (base.size() && base.back() == '/' && target.size() && target.front() == '/'
                         ? target.substr(1)
                         : target);
  text(target, 16384, "URL");
  if (target.rfind("http://", 0) != 0 && target.rfind("https://", 0) != 0)
    throw dslvm::Error("only HTTP and HTTPS URLs are supported");
  {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> u(curl_url(), curl_url_cleanup);
    if (u && curl_url_set(u.get(), CURLUPART_URL, target.c_str(), 0) == CURLUE_OK) {
      char *host = nullptr;
      if (curl_url_get(u.get(), CURLUPART_HOST, &host, 0) == CURLUE_OK) {
        std::string h = lower(host);
        curl_free(host);
        if (!request.llm_budgeted && request.verb != "GET" && request.verb != "HEAD" &&
            (h == "openrouter.ai" || h == "api.deepseek.com" || h == "ollama.com" ||
             h == "api.ollama.com"))
          throw dslvm::Error("LLM provider writes require the cost-controlled llm API");
      }
    }
  }
  if (request.verb.empty() ||
      request.verb.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_") !=
          std::string::npos)
    throw dslvm::Error("invalid HTTP method");
  auto payload = request.body;
  if (request.parts.size() > 32 || request.headers.size() > 128 || request.form.size() > 256)
    throw dslvm::Error("HTTP field count limit exceeded");
  std::size_t upload_bytes = payload.size();
  for (const auto &part : request.parts) {
    std::size_t n = part.file ? static_cast<std::size_t>(std::filesystem::file_size(part.value))
                              : part.value.size();
    if (n > 8 * 1024 * 1024 - upload_bytes)
      throw dslvm::Error("multipart upload size limit exceeded");
    upload_bytes += n;
  }
  if (payload.size() > 8 * 1024 * 1024)
    throw dslvm::Error("request body size limit exceeded");
  Response result;
  for (int64_t attempt = 0; attempt <= request.retries; ++attempt) {
    Reservation foreign(2 * 1024 * 1024 + upload_bytes);
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle)
      throw std::bad_alloc();
    auto *c = handle.get();
    Transfer t;
    t.max = static_cast<std::size_t>(request.max_bytes);
    t.started = Clock::now();
    t.result.runtime = runtime;
    t.result.url = target;
    t.result.attempts = attempt + 1;
    auto set = [&](CURLoption option, auto value) {
      auto code = curl_easy_setopt(c, option, value);
      if (code != CURLE_OK)
        throw dslvm::Error(std::string("libcurl option: ") + curl_easy_strerror(code));
    };
    set(CURLOPT_URL, target.c_str());
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(CURLOPT_NOSIGNAL, 1L);
    auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(request.deadline - Clock::now())
            .count();
    if (remaining < 1)
      throw dslvm::Error("HTTP request deadline exceeded");
    set(CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
    set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min<int64_t>(request.timeout_ms, 5000)));
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_MAXREDIRS, 5L);
    set(CURLOPT_UNRESTRICTED_AUTH, 0L);
    set(CURLOPT_ACCEPT_ENCODING, "");
    set(CURLOPT_SHARE, state->handle);
    set(CURLOPT_COOKIEFILE, "");
    set(CURLOPT_CERTINFO, 1L);
    set(CURLOPT_SSL_VERIFYPEER, request.insecure ? 0L : 1L);
    set(CURLOPT_SSL_VERIFYHOST, request.insecure ? 0L : 2L);
    set(CURLOPT_HTTP_VERSION, request.http2 ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1);
    set(CURLOPT_WRITEFUNCTION, body_cb);
    set(CURLOPT_WRITEDATA, &t);
    set(CURLOPT_HEADERFUNCTION, header_cb);
    set(CURLOPT_HEADERDATA, &t);
    set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(request.max_bytes));
    if (request.proxy_set || !request.proxy.empty())
      set(CURLOPT_PROXY, request.proxy.c_str());
    if (!request.cacert.empty())
      set(CURLOPT_CAINFO, request.cacert.c_str());
    if (!request.client_cert.empty())
      set(CURLOPT_SSLCERT, request.client_cert.c_str());
    if (!request.client_key.empty())
      set(CURLOPT_SSLKEY, request.client_key.c_str());
    if (!request.auth_kind.empty()) {
      std::string raw = request.auth_user + ":" + request.auth_password;
      std::string encoded(4 * ((raw.size() + 2) / 3), '\0');
      EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                      reinterpret_cast<const unsigned char *>(raw.data()),
                      static_cast<int>(raw.size()));
      runtime->secret(encoded);
      set(CURLOPT_HTTPAUTH, request.auth_kind == "digest" ? CURLAUTH_DIGEST : CURLAUTH_BASIC);
      set(CURLOPT_USERNAME, request.auth_user.c_str());
      set(CURLOPT_PASSWORD, request.auth_password.c_str());
    }
    SList resolve;
    if (!request.resolve.empty()) {
      text(request.resolve, 1024, "resolve override");
      resolve.add(request.resolve);
      set(CURLOPT_RESOLVE, resolve.p);
    }
    auto all = request.headers;
    if (!request.form.empty()) {
      payload.clear();
      for (const auto &kv : request.form) {
        if (!payload.empty())
          payload += '&';
        payload += escape(c, kv.first) + "=" + escape(c, kv.second);
      }
      all["content-type"] = "application/x-www-form-urlencoded";
    }
    SList hs;
    for (const auto &kv : all) {
      auto val = kv.second;
      header(kv.first, val);
      mark(*runtime, kv.first, val);
      hs.add(kv.first + ": " + val);
    }
    if (request.stream && !all.count("accept"))
      all["accept"] = "text/event-stream";
    if (!all.count("expect"))
      hs.add("Expect:");
    set(CURLOPT_HTTPHEADER, hs.p);
    std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime(nullptr, curl_mime_free);
    if (!request.parts.empty()) {
      mime.reset(curl_mime_init(c));
      if (!mime)
        throw std::bad_alloc();
      for (const auto &part : request.parts) {
        auto *p = curl_mime_addpart(mime.get());
        if (!p)
          throw std::bad_alloc();
        curl_mime_name(p, part.name.c_str());
        if (part.file) {
          auto content = read_file(part.value, 8 * 1024 * 1024);
          curl_mime_data(p, content.data(), content.size());
          curl_mime_filename(p, part.filename.empty() ? "upload.bin" : part.filename.c_str());
        } else {
          auto value = part.value;
          curl_mime_data(p, value.data(), value.size());
        }
      }
      set(CURLOPT_MIMEPOST, mime.get());
    } else if (request.verb != "GET" && request.verb != "HEAD") {
      set(CURLOPT_POSTFIELDS, payload.data());
      set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    }
    set(CURLOPT_CUSTOMREQUEST, request.verb.c_str());
    if (request.verb == "HEAD")
      set(CURLOPT_NOBODY, 1L);
    auto rc = curl_easy_perform(c);
    if (t.exception)
      std::rethrow_exception(t.exception);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    t.result.status = status;
    t.result.size = static_cast<int64_t>(t.result.body.size());
    char *effective = nullptr;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective)
      t.result.url = effective;
    auto timing = [&](CURLINFO field) {
      double value = 0;
      curl_easy_getinfo(c, field, &value);
      return value * 1000;
    };
    t.result.total_ms = timing(CURLINFO_TOTAL_TIME);
    t.result.dns_ms = timing(CURLINFO_NAMELOOKUP_TIME);
    t.result.connect_ms = timing(CURLINFO_CONNECT_TIME);
    t.result.tls_ms = timing(CURLINFO_APPCONNECT_TIME);
    t.result.ttfb_ms = timing(CURLINFO_STARTTRANSFER_TIME);
    curl_certinfo *certs = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_CERTINFO, &certs) == CURLE_OK && certs) {
      for (int i = 0; i < certs->num_of_certs && t.result.certificate.size() < 65536; ++i)
        for (auto *item = certs->certinfo[i]; item && t.result.certificate.size() < 65536;
             item = item->next)
          t.result.certificate += std::string(item->data) + "\n";
    }
    if (rc == CURLE_FILESIZE_EXCEEDED)
      throw dslvm::Error("HTTP response size limit exceeded");
    if (rc != CURLE_OK)
      t.result.error = runtime->redact(curl_easy_strerror(rc));
    curl_slist *cookie_list = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_COOKIELIST, &cookie_list) == CURLE_OK) {
      std::size_t count = 0, bytes = 0;
      for (auto *item = cookie_list; item; item = item->next) {
        ++count;
        bytes += std::strlen(item->data);
      }
      curl_slist_free_all(cookie_list);
      if (count > 64 || bytes > 256 * 1024) {
        curl_easy_setopt(c, CURLOPT_COOKIELIST, "ALL");
        throw dslvm::Error("session cookie jar limit exceeded (64 cookies / 256 KiB)");
      }
    }
    auto ck = t.result.headers.find("set-cookie");
    if (ck != t.result.headers.end()) {
      std::istringstream lines(ck->second);
      std::string line;
      while (std::getline(lines, line)) {
        auto eq = line.find('=');
        auto semi = line.find(';', eq);
        if (eq != std::string::npos)
          runtime->secret(line.substr(eq + 1, semi - eq - 1));
      }
    }
    auto j = parse_json(t.result.body, false);
    if (!j.is_discarded())
      runtime->discover_secrets(j);
    result = std::move(t.result);
    if (result.error.empty() && result.status < 500 && result.status != 429)
      break;
    if (attempt < request.retries)
      std::this_thread::sleep_for(std::chrono::milliseconds(request.retry_ms));
  }
  if (!result.error.empty())
    throw dslvm::Error("HTTP transport failed: " + result.error);
  if (request.follow &&
      (result.status == 301 || result.status == 302 || result.status == 303 ||
       result.status == 307 || result.status == 308) &&
      result.headers.count("location")) {
    if (request.redirects_left == 0)
      throw dslvm::Error("HTTP redirect limit exceeded");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> u(curl_url(), curl_url_cleanup);
    if (!u || curl_url_set(u.get(), CURLUPART_URL, target.c_str(), 0) != CURLUE_OK)
      throw dslvm::Error("invalid redirect base URL");
    auto get = [&](CURLUPart part) {
      char *p = nullptr;
      if (curl_url_get(u.get(), part, &p, CURLU_DEFAULT_PORT) != CURLUE_OK)
        return std::string();
      std::string value(p);
      curl_free(p);
      return value;
    };
    auto origin = get(CURLUPART_SCHEME) + "://" + get(CURLUPART_HOST) + ":" + get(CURLUPART_PORT);
    if (curl_url_set(u.get(), CURLUPART_URL, result.headers.at("location").c_str(), 0) != CURLUE_OK)
      throw dslvm::Error("invalid redirect URL");
    if (!get(CURLUPART_USER).empty() || !get(CURLUPART_PASSWORD).empty())
      throw dslvm::Error("redirect URL credentials are forbidden");
    request.url = get(CURLUPART_URL);
    --request.redirects_left;
    if (origin != get(CURLUPART_SCHEME) + "://" + get(CURLUPART_HOST) + ":" + get(CURLUPART_PORT)) {
      if (request.llm_budgeted)
        throw dslvm::Error("LLM redirects must remain on the configured provider origin");
      for (auto it = request.headers.begin(); it != request.headers.end();) {
        auto key = lower(it->first);
        if (key == "authorization" || key == "proxy-authorization" || key == "cookie" ||
            key.find("key") != std::string::npos || key.find("token") != std::string::npos ||
            key.find("secret") != std::string::npos)
          it = request.headers.erase(it);
        else
          ++it;
      }
      request.auth_kind.clear();
      request.auth_user.clear();
      request.auth_password.clear();
      request.client_cert.clear();
      request.client_key.clear();
    }
    if ((result.status == 303 && request.verb != "HEAD") ||
        ((result.status == 301 || result.status == 302) && request.verb == "POST")) {
      request.verb = "GET";
      request.body.clear();
      request.form.clear();
      request.parts.clear();
    }
    auto next = perform(request);
    next.total_ms += result.total_ms;
    next.attempts += result.attempts;
    return next;
  }
  return result;
}
void register_http(dslvm::Registry &r, Runtime &rt) {
  r.context("curl", "Curl");
  method(r, rt, "Curl", "session", {"string"}, "Session", [&rt](void *, Args &a) {
    return Value::object("Session", rt.hold<Session>(&rt, a[0].s));
  });
  method(r, rt, "Curl", "mock", {}, "Mock",
         [&rt](void *, Args &) { return Value::object("Mock", rt.hold<Mock>(&rt)); });
  method(r, rt, "Curl", "env", {"string"}, "string",
         [&rt](void *, Args &a) { return Value::string(rt.env_secret(a[0].s)); });
  method(r, rt, "Curl", "version", {}, "string",
         [](void *, Args &) { return Value::string(curl_version()); });
  method(r, rt, "Mock", "url", {}, "string",
         [](void *p, Args &) { return Value::string(static_cast<Mock *>(p)->url()); });
  method(r, rt, "Mock", "stop", {}, "void", [](void *p, Args &) {
    static_cast<Mock *>(p)->stop();
    return Value::none();
  });
  method(r, rt, "Mock", "serve", {"string", "string", "int"}, "void", [](void *p, Args &a) {
    static_cast<Mock *>(p)->serve(a[0].s, a[1].s, a[2].i);
    return Value::none();
  });
  method(r, rt, "Session", "request", {"string", "string"}, "Request", [&rt](void *p, Args &a) {
    return Value::object("Request", rt.hold<Request>(static_cast<Session *>(p), a[0].s, a[1].s));
  });
  for (const auto *v : {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"}) {
    auto name = lower(v);
    method(r, rt, "Session", name.c_str(), {"string"}, "Request", [&rt, v](void *p, Args &a) {
      return Value::object("Request", rt.hold<Request>(static_cast<Session *>(p), v, a[0].s));
    });
  }
  method(r, rt, "Session", "header", {"string", "string"}, "Session", [&rt](void *p, Args &a) {
    header(a[0].s, a[1].s);
    mark(rt, a[0].s, a[1].s);
    auto &s = *static_cast<Session *>(p);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.headers[lower(a[0].s)] = a[1].s;
    return Value::object("Session", p);
  });
  method(r, rt, "Session", "set", {"string", "string"}, "Session", [](void *p, Args &a) {
    auto &s = *static_cast<Session *>(p);
    text(a[0].s, 128, "variable name");
    std::lock_guard<std::mutex> lock(s.mutex);
    s.variables[a[0].s] = a[1].s;
    return Value::object("Session", p);
  });
  method(r, rt, "Session", "secret", {"string", "string"}, "Session", [&rt](void *p, Args &a) {
    auto &s = *static_cast<Session *>(p);
    auto value = rt.env_secret(a[1].s);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.variables[a[0].s] = value;
    return Value::object("Session", p);
  });
  method(r, rt, "Session", "get_var", {"string"}, "string", [](void *p, Args &a) {
    auto &s = *static_cast<Session *>(p);
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.variables.find(a[0].s);
    if (it == s.variables.end())
      throw dslvm::Error("unknown session variable");
    return Value::string(it->second);
  });
  method(r, rt, "Request", "header", {"string", "string"}, "Request", [&rt](void *p, Args &a) {
    header(a[0].s, a[1].s);
    mark(rt, a[0].s, a[1].s);
    static_cast<Request *>(p)->headers[lower(a[0].s)] = a[1].s;
    return Value::object("Request", p);
  });
  for (const auto *name : {"body", "json", "proxy", "cacert", "resolve"})
    method(r, rt, "Request", name, {"string"}, "Request", [name](void *p, Args &a) {
      auto &q = *static_cast<Request *>(p);
      std::string n = name;
      if (n == "body" || n == "json") {
        if (n == "json") {
          auto j = parse_json(a[0].s);
          (void)j;
          q.headers["content-type"] = "application/json";
        }
        q.body = a[0].s;
      } else if (n == "proxy") {
        q.proxy = a[0].s;
        q.proxy_set = true;
      } else if (n == "cacert")
        q.cacert = a[0].s;
      else
        q.resolve = a[0].s;
      return Value::object("Request", p);
    });
  method(r, rt, "Request", "form", {"string", "string"}, "Request", [](void *p, Args &a) {
    static_cast<Request *>(p)->form[a[0].s] = a[1].s;
    return Value::object("Request", p);
  });
  method(r, rt, "Request", "part", {"string", "string"}, "Request", [](void *p, Args &a) {
    static_cast<Request *>(p)->parts.push_back({a[0].s, a[1].s, "", false});
    return Value::object("Request", p);
  });
  method(r, rt, "Request", "upload", {"string", "string", "string"}, "Request",
         [](void *p, Args &a) {
           static_cast<Request *>(p)->parts.push_back({a[0].s, a[1].s, a[2].s, true});
           return Value::object("Request", p);
         });
  method(r, rt, "Request", "cert", {"string", "string"}, "Request", [](void *p, Args &a) {
    auto &q = *static_cast<Request *>(p);
    q.client_cert = a[0].s;
    q.client_key = a[1].s;
    return Value::object("Request", p);
  });
  for (const auto *kind : {"basic", "digest"})
    method(r, rt, "Request", kind, {"string", "string"}, "Request", [&rt, kind](void *p, Args &a) {
      auto &q = *static_cast<Request *>(p);
      text(a[0].s, 8192, "auth user");
      text(a[1].s, 8192, "auth password");
      q.auth_user = a[0].s;
      q.auth_password = a[1].s;
      q.auth_kind = kind;
      rt.secret(a[1].s);
      return Value::object("Request", p);
    });
  method(r, rt, "Request", "bearer_env", {"string"}, "Request", [&rt](void *p, Args &a) {
    auto token = rt.env_secret(a[0].s);
    if (token.empty())
      throw dslvm::Error("credential environment variable is not set");
    static_cast<Request *>(p)->headers["authorization"] = "Bearer " + token;
    return Value::object("Request", p);
  });
  for (const auto *name : {"timeout", "limit"})
    method(r, rt, "Request", name, {"int"}, "Request", [name](void *p, Args &a) {
      auto &q = *static_cast<Request *>(p);
      if (std::string(name) == "timeout")
        q.timeout_ms = a[0].i;
      else
        q.max_bytes = a[0].i;
      return Value::object("Request", p);
    });
  method(r, rt, "Request", "retry", {"int", "int"}, "Request", [](void *p, Args &a) {
    auto &q = *static_cast<Request *>(p);
    q.retries = a[0].i;
    q.retry_ms = a[1].i;
    return Value::object("Request", p);
  });
  for (const auto *name : {"follow", "insecure", "http2", "stream"})
    method(r, rt, "Request", name, {"bool"}, "Request", [name](void *p, Args &a) {
      auto &q = *static_cast<Request *>(p);
      std::string n = name;
      if (n == "follow")
        q.follow = a[0].b;
      else if (n == "insecure")
        q.insecure = a[0].b;
      else if (n == "http2")
        q.http2 = a[0].b;
      else
        q.stream = a[0].b;
      return Value::object("Request", p);
    });
  method(r, rt, "Request", "send", {}, "Response", [&rt](void *p, Args &) {
    return Value::object("Response", rt.hold<Response>(static_cast<Request *>(p)->send()));
  });
  method(r, rt, "Response", "status", {}, "int",
         [](void *p, Args &) { return Value::integer(static_cast<Response *>(p)->status); });
  method(r, rt, "Response", "bytes", {}, "int",
         [](void *p, Args &) { return Value::integer(static_cast<Response *>(p)->size); });
  for (const auto *name : {"body", "error", "url"})
    method(r, rt, "Response", name, {}, "string", [name](void *p, Args &) {
      auto &v = *static_cast<Response *>(p);
      return Value::string(std::string(name) == "body"  ? v.body
                           : std::string(name) == "url" ? v.url
                                                        : v.error);
    });
  method(r, rt, "Response", "header", {"string"}, "string", [](void *p, Args &a) {
    auto &h = static_cast<Response *>(p)->headers;
    auto it = h.find(lower(a[0].s));
    return Value::string(it == h.end() ? "" : it->second);
  });
  method(r, rt, "Response", "cookie", {"string"}, "string", [](void *p, Args &a) {
    std::string raw = static_cast<Response *>(p)->headers["set-cookie"];
    std::size_t pos = 0;
    while (pos < raw.size()) {
      auto end = raw.find('\n', pos);
      auto line = raw.substr(pos, end - pos);
      auto eq = line.find('=');
      if (line.substr(0, eq) == a[0].s) {
        auto semi = line.find(';', eq);
        return Value::string(line.substr(eq + 1, semi - eq - 1));
      }
      if (end == std::string::npos)
        break;
      pos = end + 1;
    }
    return Value::string("");
  });
  method(r, rt, "Response", "time", {"string"}, "float", [](void *p, Args &a) {
    auto &v = *static_cast<Response *>(p);
    auto n = a[0].s;
    if (n == "total")
      return Value::real(v.total_ms);
    if (n == "dns")
      return Value::real(v.dns_ms);
    if (n == "connect")
      return Value::real(v.connect_ms);
    if (n == "tls")
      return Value::real(v.tls_ms);
    if (n == "ttfb")
      return Value::real(v.ttfb_ms);
    if (n == "first_data")
      return Value::real(v.first_data_ms);
    throw dslvm::Error("unknown timing metric");
  });
  method(r, rt, "Response", "save", {"string"}, "void", [&rt](void *p, Args &a) {
    rt.write(a[0].s, static_cast<Response *>(p)->body);
    return Value::none();
  });
  method(r, rt, "Response", "attempts", {}, "int",
         [](void *p, Args &) { return Value::integer(static_cast<Response *>(p)->attempts); });
  method(r, rt, "Response", "certificate", {}, "string",
         [](void *p, Args &) { return Value::string(static_cast<Response *>(p)->certificate); });
  method(r, rt, "Response", "snapshot", {"string", "bool"}, "bool", [&rt](void *p, Args &a) {
    auto text = rt.redact(static_cast<Response *>(p)->body);
    if (a[1].b) {
      rt.write(a[0].s, text);
      return Value::boolean(true);
    }
    try {
      return Value::boolean(read_file(a[0].s) == text);
    } catch (...) {
      return Value::boolean(false);
    }
  });
  method(r, rt, "Response", "sse_count", {}, "int", [](void *p, Args &) {
    std::istringstream in(static_cast<Response *>(p)->body);
    std::string line;
    int n = 0;
    while (std::getline(in, line))
      if (line.rfind("data:", 0) == 0 && trim(line.substr(5)) != "[DONE]")
        ++n;
    return Value::integer(n);
  });
}
} // namespace lmcurl
