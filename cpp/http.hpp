// Minimal HTTP/1.1 client over POSIX sockets (Connection: close), optional TLS through OpenSSL when HAVE_OPENSSL is
// defined by the Makefile (headers and libssl present). Content-Length and chunked bodies, bounded redirects, timeouts
// and size limits. No dependencies beyond the standard library, POSIX and (optionally) OpenSSL.
#pragma once
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>
#ifdef HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace http {

struct Error : std::runtime_error { explicit Error(const std::string& m) : std::runtime_error(m) {} };

struct Url { std::string scheme, host, path; int port = 80; };

inline std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

inline Url parse_url(const std::string& text) {
  Url u; size_t p = text.find("://");
  if (p == std::string::npos) throw Error("url without scheme: " + text);
  u.scheme = lower(text.substr(0, p));
  if (u.scheme != "http" && u.scheme != "https") throw Error("unsupported scheme '" + u.scheme + "' in " + text);
  u.port = u.scheme == "https" ? 443 : 80;
  size_t hostStart = p + 3, slash = text.find_first_of("/?#", hostStart);
  std::string authority = text.substr(hostStart, slash == std::string::npos ? std::string::npos : slash - hostStart);
  if (authority.find('@') != std::string::npos) throw Error("userinfo in url is not supported: " + text);
  size_t colon = authority.rfind(':');
  if (colon != std::string::npos) { u.port = std::stoi(authority.substr(colon + 1)); authority = authority.substr(0, colon); }
  if (authority.empty()) throw Error("url without host: " + text);
  u.host = lower(authority);
  u.path = slash == std::string::npos ? "/" : text.substr(slash);
  if (u.path.empty() || u.path[0] == '?') u.path = "/" + u.path;
  size_t hash = u.path.find('#'); if (hash != std::string::npos) u.path.erase(hash);  // fragments never go on the wire
  return u;
}

// Resolves a (possibly relative) reference against a base, RFC 3986 lite: absolute URLs, scheme-relative, root-relative,
// relative paths with ./ and ../, query-only. Good enough for static sites; anything odd is just a string the client rejects.
inline std::string resolve(const std::string& base, const std::string& ref) {
  if (ref.find("://") != std::string::npos) return ref;
  Url b = parse_url(base);
  std::string origin = b.scheme + "://" + b.host + ((b.scheme == "http" && b.port == 80) || (b.scheme == "https" && b.port == 443) ? "" : ":" + std::to_string(b.port));
  if (ref.rfind("//", 0) == 0) return b.scheme + ":" + ref;
  if (ref.empty()) return origin + b.path;
  if (ref[0] == '#') return origin + b.path;
  if (ref[0] == '/') return origin + ref;
  std::string path = b.path; size_t q = path.find('?'); if (q != std::string::npos) path.erase(q);
  if (ref[0] == '?') return origin + path + ref;
  path.erase(path.rfind('/') + 1);
  std::string merged = path + ref, query;
  q = merged.find('?'); if (q != std::string::npos) { query = merged.substr(q); merged.erase(q); }
  std::vector<std::string> segments; size_t i = (!merged.empty() && merged[0] == '/') ? 1 : 0;
  while (i <= merged.size()) {
    size_t j = merged.find('/', i); std::string seg = merged.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (seg == "..") { if (!segments.empty()) segments.pop_back(); }
    else if (seg != "." && !seg.empty()) segments.push_back(seg);
    if (j == std::string::npos) break;
    i = j + 1;
  }
  std::string out; for (size_t k = 0; k < segments.size(); ++k) out += "/" + segments[k];
  if (out.empty()) out = "/";
  bool dirLike = merged.back() == '/' || (merged.size() >= 3 && merged.compare(merged.size()-3, 3, "/..") == 0) || (merged.size() >= 2 && merged.compare(merged.size()-2, 2, "/.") == 0);
  if (segments.size() && dirLike && out.back() != '/') out += "/";
  return origin + out + query;
}

struct Response { int status = 0; std::vector<std::pair<std::string, std::string>> headers; std::string body; std::string finalUrl;
  std::string header(const std::string& name) const { std::string n = lower(name); for (auto& h : headers) if (lower(h.first) == n) return h.second; return ""; } };

struct Options { int timeoutMs = 10000; size_t maxBytes = 8u * 1024u * 1024u; int maxRedirects = 5; };

class Connection {
 public:
  Connection(const Url& u, int timeoutMs) {
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC; addrinfo* res = nullptr;
    int rc = ::getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res);
    if (rc != 0) throw Error("cannot resolve " + u.host + ": " + gai_strerror(rc));
    for (addrinfo* a = res; a; a = a->ai_next) {
      fd_ = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol); if (fd_ < 0) continue;
      timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000}; ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
      if (::connect(fd_, a->ai_addr, a->ai_addrlen) == 0) break;
      ::close(fd_); fd_ = -1;
    }
    ::freeaddrinfo(res);
    if (fd_ < 0) throw Error("cannot connect to " + u.host + ":" + std::to_string(u.port));
    if (u.scheme == "https") {
#ifdef HAVE_OPENSSL
      static bool init = (SSL_library_init(), SSL_load_error_strings(), true); (void)init;
      ctx_ = SSL_CTX_new(TLS_client_method()); SSL_CTX_set_default_verify_paths(ctx_); SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
      ssl_ = SSL_new(ctx_); SSL_set_fd(ssl_, fd_); SSL_set_tlsext_host_name(ssl_, u.host.c_str()); SSL_set1_host(ssl_, u.host.c_str());
      if (SSL_connect(ssl_) != 1) { unsigned long e = ERR_get_error(); char buf[256]; ERR_error_string_n(e, buf, sizeof buf); throw Error("TLS handshake with " + u.host + " failed: " + buf); }
#else
      throw Error("https is not available: this host was built without OpenSSL (http:// only)");
#endif
    }
  }
  ~Connection() {
#ifdef HAVE_OPENSSL
    if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); } if (ctx_) SSL_CTX_free(ctx_);
#endif
    if (fd_ >= 0) ::close(fd_);
  }
  void write_all(const std::string& s) { size_t off = 0; while (off < s.size()) { ssize_t n = send_(s.data() + off, s.size() - off); if (n <= 0) throw Error("send failed"); off += size_t(n); } }
  // Returns 0 at end of stream; throws on timeout or error.
  ssize_t read_some(char* buf, size_t n) { ssize_t r = recv_(buf, n); if (r < 0) throw Error("receive failed or timed out"); return r; }
 private:
  ssize_t send_(const char* p, size_t n) {
#ifdef HAVE_OPENSSL
    if (ssl_) return SSL_write(ssl_, p, int(n));
#endif
    return ::send(fd_, p, n, 0); }
  ssize_t recv_(char* p, size_t n) {
#ifdef HAVE_OPENSSL
    if (ssl_) { int r = SSL_read(ssl_, p, int(n)); if (r <= 0 && SSL_get_error(ssl_, r) == SSL_ERROR_ZERO_RETURN) return 0; return r; }
#endif
    return ::recv(fd_, p, n, 0); }
  int fd_ = -1;
#ifdef HAVE_OPENSSL
  SSL_CTX* ctx_ = nullptr; SSL* ssl_ = nullptr;
#endif
};

inline Response fetch_once(const std::string& url, const std::string& method, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body, const Options& opt) {
  Url u = parse_url(url);
  Connection c(u, opt.timeoutMs);
  std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ((u.scheme == "http" && u.port == 80) || (u.scheme == "https" && u.port == 443) ? "" : ":" + std::to_string(u.port)) + "\r\nConnection: close\r\nUser-Agent: lmcurl/0.1\r\n";
  bool hasLength = false;
  for (auto& h : headers) { req += h.first + ": " + h.second + "\r\n"; if (lower(h.first) == "content-length") hasLength = true; }
  if ((!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") && !hasLength) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  req += "\r\n" + body;
  c.write_all(req);
  std::string data; char buf[16384]; size_t headEnd;
  while ((headEnd = data.find("\r\n\r\n")) == std::string::npos) {
    ssize_t n = c.read_some(buf, sizeof buf); if (n == 0) throw Error("connection closed before the response headers");
    data.append(buf, size_t(n)); if (data.size() > 1u << 20) throw Error("response headers exceed 1 MiB");
  }
  Response r; r.finalUrl = url;
  std::string head = data.substr(0, headEnd); data.erase(0, headEnd + 4);
  size_t eol = head.find("\r\n"); std::string statusLine = head.substr(0, eol);
  if (statusLine.compare(0, 5, "HTTP/") != 0 || statusLine.size() < 12) throw Error("malformed status line: " + statusLine);
  r.status = std::stoi(statusLine.substr(9, 3));
  size_t pos = eol == std::string::npos ? head.size() : eol + 2;
  while (pos < head.size()) { size_t e = head.find("\r\n", pos); std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos); size_t colon = line.find(':'); if (colon != std::string::npos) { std::string v = line.substr(colon + 1); while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin()); r.headers.emplace_back(line.substr(0, colon), v); } if (e == std::string::npos) break; pos = e + 2; }
  auto readMore = [&]() { ssize_t n = c.read_some(buf, sizeof buf); if (n > 0) data.append(buf, size_t(n)); if (data.size() > opt.maxBytes + 65536) throw Error("response exceeds the size limit of " + std::to_string(opt.maxBytes) + " bytes"); return n > 0; };
  if (lower(r.header("Transfer-Encoding")).find("chunked") != std::string::npos) {
    while (true) {
      size_t e; while ((e = data.find("\r\n")) == std::string::npos) if (!readMore()) throw Error("connection closed inside a chunked body");
      size_t size = std::stoul(data.substr(0, e), nullptr, 16); data.erase(0, e + 2);
      if (size == 0) break;
      while (data.size() < size + 2) if (!readMore()) throw Error("connection closed inside a chunk");
      r.body.append(data, 0, size); data.erase(0, size + 2);
      if (r.body.size() > opt.maxBytes) throw Error("response exceeds the size limit of " + std::to_string(opt.maxBytes) + " bytes");
    }
  } else if (!r.header("Content-Length").empty()) {
    size_t want = std::stoul(r.header("Content-Length"));
    if (want > opt.maxBytes) throw Error("response of " + std::to_string(want) + " bytes exceeds the size limit of " + std::to_string(opt.maxBytes));
    while (data.size() < want) if (!readMore()) throw Error("connection closed before the full body (" + std::to_string(data.size()) + " of " + std::to_string(want) + " bytes)");
    r.body = data.substr(0, want);
  } else if (method != "HEAD" && r.status != 204 && r.status != 304) {
    while (readMore()) {}
    r.body = data; if (r.body.size() > opt.maxBytes) throw Error("response exceeds the size limit of " + std::to_string(opt.maxBytes) + " bytes");
  }
  return r;
}

// Follows 301/302/303/307/308 up to opt.maxRedirects hops; when the limit is reached the last 3xx response is returned as-is
// (max_redirects(0) behaves like curl without -L). 303 and 301/302 on POST switch to GET without a body (browsers' rule).
inline Response fetch(std::string url, std::string method, const std::vector<std::pair<std::string, std::string>>& headers, std::string body, const Options& opt) {
  for (int hop = 0; ; ++hop) {
    Response r = fetch_once(url, method, headers, body, opt);
    bool redirect = r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308;
    if (!redirect || r.header("Location").empty() || hop >= opt.maxRedirects) return r;
    url = resolve(url, r.header("Location"));
    if (r.status == 303 || ((r.status == 301 || r.status == 302) && method == "POST")) { method = "GET"; body.clear(); }
  }
}

}  // namespace http
