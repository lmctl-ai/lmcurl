#include "core.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <deque>
#include <condition_variable>
#include <thread>
#include <filesystem>
#include <fstream>
#include <zlib.h>
namespace lmcurl {
namespace {
std::string decode(std::string s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto pair = s.substr(i + 1, 2);
      char *end = nullptr;
      long n = strtol(pair.c_str(), &end, 16);
      if (end && !*end) {
        out += static_cast<char>(n);
        i += 2;
        continue;
      }
    }
    out += s[i] == '+' ? ' ' : s[i];
  }
  return out;
}
Json pairs(const std::string &s, char delimiter = '&') {
  Json out = Json::object();
  std::size_t pos = 0;
  while (pos < s.size()) {
    auto end = s.find(delimiter, pos);
    auto item = s.substr(pos, end - pos);
    auto eq = item.find('=');
    out[decode(trim(item.substr(0, eq)))] =
        decode(eq == std::string::npos ? "" : trim(item.substr(eq + 1)));
    if (end == std::string::npos)
      break;
    pos = end + 1;
  }
  return out;
}
bool send_all(int fd, const std::string &s) {
  std::size_t pos = 0;
  while (pos < s.size()) {
    auto n = send(fd, s.data() + pos, s.size() - pos, MSG_NOSIGNAL);
    if (n <= 0)
      return false;
    pos += static_cast<std::size_t>(n);
  }
  return true;
}
} // namespace
struct MockState {
  Runtime *rt;
  int listen_fd = -1;
  unsigned port = 0;
  std::atomic<bool> stopping{false};
  std::thread accept_thread;
  std::vector<std::thread> workers;
  std::mutex mutex;
  std::vector<int> sockets;
  std::deque<int> queue;
  std::condition_variable ready;
  std::map<std::string, int> counters;
  Json users = Json::object();
  int next_user = 1;
  explicit MockState(Runtime *r) : rt(r) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
      throw dslvm::Error("cannot create mock socket");
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
        listen(listen_fd, 32)) {
      close(listen_fd);
      throw dslvm::Error("cannot bind loopback mock");
    }
    socklen_t len = sizeof(address);
    getsockname(listen_fd, reinterpret_cast<sockaddr *>(&address), &len);
    port = ntohs(address.sin_port);
    try {
      for (int i = 0; i < 16; ++i)
        workers.emplace_back([this] {
          while (true) {
            int fd;
            {
              std::unique_lock<std::mutex> lock(mutex);
              ready.wait(lock, [this] { return stopping || !queue.empty(); });
              if (queue.empty()) {
                if (stopping)
                  return;
                continue;
              }
              fd = queue.front();
              queue.pop_front();
            }
            try {
              handle(fd);
            } catch (...) {
            }
            shutdown(fd, SHUT_RDWR);
            close(fd);
            {
              std::lock_guard<std::mutex> lock(mutex);
              sockets.erase(std::remove(sockets.begin(), sockets.end(), fd), sockets.end());
            }
          }
        });
      accept_thread = std::thread([this] {
        while (!stopping) {
          pollfd p{listen_fd, POLLIN, 0};
          if (poll(&p, 1, 50) <= 0)
            continue;
          int fd = accept(listen_fd, nullptr, nullptr);
          if (fd < 0)
            continue;
          {
            std::lock_guard<std::mutex> lock(mutex);
            if (sockets.size() >= 64) {
              close(fd);
              continue;
            }
            sockets.push_back(fd);
            queue.push_back(fd);
          }
          ready.notify_one();
        }
      });
    } catch (...) {
      stop();
      throw;
    }
  }
  void stop() {
    if (stopping.exchange(true))
      return;
    shutdown(listen_fd, SHUT_RDWR);
    if (accept_thread.joinable())
      accept_thread.join();
    ready.notify_all();
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (int fd : sockets)
        shutdown(fd, SHUT_RDWR);
    }
    for (auto &t : workers)
      if (t.joinable())
        t.join();
    close(listen_fd);
    listen_fd = -1;
  }
  ~MockState() { stop(); }
  void handle(int fd) {
    timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::string data;
    char buf[8192];
    std::size_t split = std::string::npos, length = 0;
    while (data.size() <= 1024 * 1024) {
      auto n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0)
        return;
      data.append(buf, static_cast<std::size_t>(n));
      split = data.find("\r\n\r\n");
      if (split != std::string::npos)
        break;
    }
    if (split == std::string::npos || split > 65536)
      return;
    auto line_end = data.find("\r\n");
    auto first = data.substr(0, line_end);
    auto sp = first.find(' '), sp2 = first.find(' ', sp + 1);
    if (sp == std::string::npos || sp2 == std::string::npos)
      return;
    auto verb = first.substr(0, sp), target = first.substr(sp + 1, sp2 - sp - 1);
    Json headers = Json::object();
    for (std::size_t p = line_end + 2; p < split;) {
      auto e = data.find("\r\n", p);
      auto line = data.substr(p, e - p);
      auto c = line.find(':');
      if (c != std::string::npos)
        headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
      p = e + 2;
    }
    if (headers.contains("content-length")) {
      auto raw = headers["content-length"].get<std::string>();
      if (raw.empty() || raw.find_first_not_of("0123456789") != std::string::npos)
        return;
      length = std::stoull(raw);
      if (length > 1024 * 1024)
        return;
    }
    while (data.size() - split - 4 < length) {
      auto n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0)
        return;
      data.append(buf, static_cast<std::size_t>(n));
      if (data.size() > 1024 * 1024 + 65536)
        return;
    }
    std::string body = data.substr(split + 4, length);
    auto q = target.find('?');
    auto path = target.substr(0, q);
    Json args = q == std::string::npos ? Json::object() : pairs(target.substr(q + 1));
    Json j = parse_json(body, false);
    if (j.is_discarded())
      j = nullptr;
    int status = 200;
    std::string content_type = "application/json", extra, payload;
    Json result;
    // Deterministic web fixtures used by lmcurl+lmhtml examples.  They are
    // deliberately served by the loopback mock so parser examples never
    // depend on a network or a mutable external site.
    if (path == "/site/index.html") {
      content_type = "text/html";
      payload =
          "<!doctype html><html><head><title>Fixture site</title></head><body><h1>Fixture site</h1>"
          "<a href=\"/site/about.html\">About</a><a href=\"/site/products.html\">Products</a>"
          "<a href=\"/site/missing.html\">Missing</a><a "
          "href=\"/site/private/secret.html\">Private</a></body></html>";
    } else if (path == "/site/about.html") {
      content_type = "text/html";
      payload =
          "<!doctype html><html><head><title>About "
          "fixture</title></head><body><h1>About</h1><p>Offline fixture site.</p></body></html>";
    } else if (path == "/site/products.html") {
      content_type = "text/html";
      payload =
          "<!doctype "
          "html><html><head><title>Products</title></head><body><h1>Products</"
          "h1><table><tr><th>SKU</th><th>Price</th><th>Stock</th></tr>"
          "<tr><td>orbit</td><td>19.50</td><td>8</td></tr><tr><td>comet</td><td>29.00</td><td>0</"
          "td></tr><tr><td>nova</td><td>12.00</td><td>4</td></tr></table></body></html>";
    } else if (path == "/site/api/products.json") {
      result = {{"products",
                 {{{"sku", "orbit"}, {"price", 19.50}, {"stock", 8}},
                  {{"sku", "comet"}, {"price", 29.00}, {"stock", 0}},
                  {{"sku", "nova"}, {"price", 12.00}, {"stock", 4}}}}};
    } else if (path == "/site/sitemap.xml") {
      content_type = "application/xml";
      auto origin = "http://127.0.0.1:" + std::to_string(port);
      payload =
          "<?xml version=\"1.0\"?><urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">";
      for (const auto *route : {"/site/index.html", "/site/about.html", "/site/products.html"})
        payload += "<url><loc>" + origin + route + "</loc></url>";
      payload += "</urlset>";
    } else if (path == "/site/feed.rss") {
      content_type = "application/rss+xml";
      auto origin = "http://127.0.0.1:" + std::to_string(port);
      payload =
          "<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>Fixture feed</title><link>" +
          origin + "/site/index.html</link><description>Offline observatory news</description>";
      payload +=
          "<item><title>Orbit launch</title><link>" + origin +
          "/site/articles/orbit</link><guid isPermaLink=\"true\">" + origin +
          "/site/articles/orbit</guid><pubDate>Mon, 01 Jan 2024 00:00:00 GMT</pubDate></item>";
      payload += "<item><title>Comet watch</title><link>" + origin +
                 "/site/articles/comet</link><guid isPermaLink=\"true\">" + origin +
                 "/site/articles/comet</guid><pubDate>Tue, 02 Jan 2024 00:00:00 "
                 "GMT</pubDate></item></channel></rss>";
    } else if (path == "/site/feed.atom") {
      content_type = "application/atom+xml";
      auto origin = "http://127.0.0.1:" + std::to_string(port);
      payload = "<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Fixture "
                "Atom</title><id>" +
                origin +
                "/site/feed.atom</id><updated>2024-01-02T00:00:00Z</updated><author><name>Fixture "
                "editor</name></author>";
      payload += "<entry><title>Orbit launch</title><link href=\"" + origin +
                 "/site/articles/orbit\"/><id>" + origin +
                 "/site/articles/orbit</id><updated>2024-01-01T00:00:00Z</updated></entry>";
      payload += "<entry><title>Comet watch</title><link href=\"" + origin +
                 "/site/articles/comet\"/><id>" + origin +
                 "/site/articles/comet</id><updated>2024-01-02T00:00:00Z</updated></entry></feed>";
    } else if (path == "/site/articles/orbit" || path == "/site/articles/comet") {
      content_type = "text/html";
      payload = "<html><head><title>Observatory news</title></head><body><article><h1>" +
                path.substr(15) + "</h1><p>Offline news.</p></article></body></html>";
    } else if (path == "/site/login.html") {
      content_type = "text/html";
      payload = "<!doctype html><html><body><form action=\"/site/login\" method=\"post\"><input "
                "type=\"hidden\" name=\"csrf_token\" value=\"fixture-csrf-public\"><input "
                "name=\"username\"><input name=\"password\" type=\"password\"><button>Sign "
                "in</button></form></body></html>";
    } else if (path == "/site/login" && verb == "POST") {
      auto form = pairs(body);
      if (form.value("csrf_token", "") == "fixture-csrf-public" &&
          form.value("username", "") == "demo" && form.value("password", "") == "demo") {
        extra = "Set-Cookie: site_session=fixture-site-session; Path=/; HttpOnly\r\n";
        result = {{"ok", true}};
      } else {
        status = 403;
        result = {{"ok", false}};
      }
    } else if (path == "/site/account.html") {
      content_type = "text/html";
      if (pairs(headers.value("cookie", std::string()), ';').value("site_session", "") !=
          "fixture-site-session") {
        status = 401;
        payload = "<html><body>Unauthorized</body></html>";
      } else
        payload = "<html><head><title>Account</title></head><body><h1>Demo account</h1><p "
                  "class=\"balance\">42.00</p></body></html>";
    } else if (path == "/site/page/1.html" || path == "/site/page/2.html" ||
               path == "/site/page/3.html") {
      content_type = "text/html";
      int page = std::stoi(path.substr(11));
      int start = (page - 1) * 3 + 1;
      payload = "<html><body><h1>Articles page " + std::to_string(page) + "</h1>";
      for (int i = 0; i < 3; ++i)
        payload += "<article><h2>Article " + std::to_string(start + i) +
                   "</h2><p>Offline content.</p></article>";
      if (page < 3)
        payload +=
            "<a rel=\"next\" href=\"/site/page/" + std::to_string(page + 1) + ".html\">Next</a>";
      payload += "</body></html>";
    } else if (path == "/site/assets/site.css") {
      content_type = "text/css";
      payload = "body{font-family:sans-serif}.item{color:#345}";
    } else if (path == "/site/assets/logo.svg") {
      content_type = "image/svg+xml";
      payload = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\"><rect "
                "width=\"40\" height=\"20\" fill=\"#345\"/></svg>";
    } else if (path == "/site/assets/app.js") {
      content_type = "text/javascript";
      payload = "console.log('fixture');";
    } else if (path == "/site/assets/missing.png") {
      status = 404;
      content_type = "image/png";
      payload = "";
    } else if (path == "/site/redirect/a") {
      status = 302;
      extra = "Location: /site/redirect/b\r\n";
      result = {{"redirect", "a"}};
    } else if (path == "/site/redirect/b") {
      status = 301;
      extra = "Location: /site/about.html\r\n";
      result = {{"redirect", "b"}};
    } else if (path == "/site/seo.html") {
      content_type = "text/html";
      payload = "<html><head><title>SEO fixture</title></head><body><h1>One</h1><h1>Two</h1><img "
                "alt=\"logo\" src=\"/site/assets/logo.svg\"><img "
                "src=\"/site/assets/missing.png\"></body></html>";
    } else if (path == "/site/a11y.html") {
      content_type = "text/html";
      payload = "<html><body><input><button></button><img src=\"/site/assets/logo.svg\"><a "
                "href=\"/site/about.html\"></a></body></html>";
    } else if (path == "/site/metadata.html") {
      content_type = "text/html";
      payload = "<html><head><meta property=\"og:title\" content=\"Fixture metadata\"><meta "
                "property=\"og:image\" content=\"/site/assets/logo.svg\"><script "
                "type=\"application/ld+json\">{\"@context\":\"https://"
                "schema.org\",\"@type\":\"Article\",\"headline\":\"Fixture\"}</script></"
                "head><body><h1>Metadata</h1></body></html>";
    } else if (path == "/site/forms.html") {
      content_type = "text/html";
      payload = "<html><body><form action=\"/site/search\" method=\"get\"><input "
                "name=\"q\"><button>Search</button></form><form action=\"/site/contact\" "
                "method=\"post\"><input name=\"email\"><textarea "
                "name=\"message\"></textarea><button>Send</button></form></body></html>";
    } else if (path == "/site/robots.txt") {
      content_type = "text/plain";
      payload = "User-agent: *\nDisallow: /site/private/\nCrawl-delay: 0.02\n";
    } else if (path == "/site/private/secret.html") {
      status = 403;
      content_type = "text/html";
      payload = "<html><body>fixture private area</body></html>";
    } else if (path == "/site/changes.html") {
      content_type = "text/html";
      int count;
      {
        std::lock_guard<std::mutex> lock(mutex);
        count = ++counters[path];
      }
      payload = "<html><body><h1>Changes</h1><p>version " + std::to_string(std::min(count, 2)) +
                "</p></body></html>";
    } else if (path == "/site/watch.html") {
      content_type = "text/html";
      int count;
      {
        std::lock_guard<std::mutex> lock(mutex);
        count = ++counters[path];
      }
      int price = count == 1 ? 25 : count == 2 ? 22 : 19;
      int stock = count < 3 ? 0 : 3;
      payload = "<html><body><span class=\"price\">" + std::to_string(price) +
                "</span><span class=\"stock\">" + std::to_string(stock) + "</span></body></html>";
    } else if (path == "/site/mirror.html") {
      content_type = "text/html";
      payload =
          "<html><head><link rel=\"stylesheet\" href=\"/site/assets/site.css\"><script "
          "src=\"/site/assets/app.js\"></script></head><body><img src=\"/site/assets/logo.svg\"><a "
          "href=\"/site/about.html\">About</a></body></html>";
    } else if (path == "/site/audit.html") {
      content_type = "text/html";
      payload = "<html><body><img alt=\"logo\" src=\"/site/assets/logo.svg\"><img "
                "src=\"/site/assets/missing.png\"><a "
                "href=\"/site/redirect/a\">redirect</a></body></html>";
    } else if (path.rfind("/site/", 0) == 0) {
      status = 404;
      content_type = "text/html";
      payload = "<html><body>Not found</body></html>";
    } else if (path.rfind("/bytes/", 0) == 0) {
      int n = std::clamp(std::stoi(path.substr(7)), 0, 1048576);
      payload = std::string(n, 'x');
      content_type = "application/octet-stream";
    } else if (path == "/gzip") {
      auto raw = Json({{"compressed", true}, {"name", "lmcurl"}}).dump();
      z_stream z{};
      if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw dslvm::Error("gzip fixture initialization failed");
      std::string out(4096, '\0');
      z.next_in = reinterpret_cast<Bytef *>(raw.data());
      z.avail_in = raw.size();
      z.next_out = reinterpret_cast<Bytef *>(out.data());
      z.avail_out = out.size();
      int rc = deflate(&z, Z_FINISH);
      if (rc != Z_STREAM_END) {
        deflateEnd(&z);
        throw dslvm::Error("gzip fixture failed");
      }
      out.resize(z.total_out);
      deflateEnd(&z);
      payload = out;
      extra = "Content-Encoding: gzip\r\n";
    } else if (path == "/json")
      result = {{"ok", true},
                {"name", "lmcurl"},
                {"items", {1, 2, 3}},
                {"user", {{"id", 7}, {"name", "Ada"}}}};
    else if (path == "/xml") {
      content_type = "application/xml";
      payload = "<catalog><item id=\"1\">alpha</item><item id=\"2\">beta</item></catalog>";
    } else if (path == "/html") {
      content_type = "text/html";
      payload = "<!doctype html><html><body><h1>lmcurl</h1><p class=\"item\">alpha</p><p "
                "class=\"item\">beta</p></body></html>";
    } else if (path.rfind("/status/", 0) == 0) {
      status = std::stoi(path.substr(8));
      if (status < 100 || status > 599)
        status = 400;
      result = {{"status", status}};
    } else if (path.rfind("/redirect/", 0) == 0) {
      int hops = std::stoi(path.substr(10));
      if (hops > 0) {
        status = 302;
        extra = "Location: /redirect/" + std::to_string(hops - 1) + "\r\n";
        result = {{"redirect", hops}};
      } else
        result = {{"redirected", true}};
    } else if (path == "/cookies/set") {
      for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key().find_first_of("\r\n;=") == std::string::npos &&
            it.value().get<std::string>().find_first_of("\r\n;") == std::string::npos)
          extra += "Set-Cookie: " + it.key() + "=" + it.value().get<std::string>() +
                   "; Path=/; HttpOnly\r\n";
      result = {{"set", true}};
    } else if (path == "/cookies")
      result = {{"cookies", pairs(headers.value("cookie", std::string()), ';')}};
    else if (path == "/login") {
      if (j.is_object() && j.value("username", "") == "demo" && j.value("password", "") == "demo") {
        result = {{"ok", true}, {"token", "fixture-token"}};
        extra = "Set-Cookie: session=fixture-session; Path=/; HttpOnly\r\n";
      } else {
        status = 401;
        result = {{"ok", false}};
      }
    } else if (path == "/protected") {
      bool ok = pairs(headers.value("cookie", std::string()), ';').value("session", "") ==
                    "fixture-session" ||
                headers.value("authorization", std::string()) == "Bearer fixture-token";
      status = ok ? 200 : 401;
      result = {{"authorized", ok}};
    } else if (path == "/auth/basic") {
      bool ok = headers.value("authorization", std::string()) == "Basic ZGVtbzpkZW1v";
      status = ok ? 200 : 401;
      if (!ok)
        extra = "WWW-Authenticate: Basic realm=\"fixture\"\r\n";
      result = {{"authorized", ok}};
    } else if (path.rfind("/flaky/", 0) == 0 || path.rfind("/poll/", 0) == 0) {
      auto slash = path.rfind('/');
      int threshold = std::stoi(path.substr(slash + 1)), count;
      {
        std::lock_guard<std::mutex> lock(mutex);
        count = ++counters[path];
      }
      bool ready = count > threshold;
      status = path.rfind("/flaky/", 0) == 0 && !ready ? 503 : 200;
      result = {{"ready", ready}, {"attempt", count}};
    } else if (path.rfind("/delay/", 0) == 0) {
      int ms = std::clamp(std::stoi(path.substr(7)), 0, 2000);
      auto end = Clock::now() + std::chrono::milliseconds(ms);
      while (!stopping && Clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      result = {{"delay_ms", ms}};
    } else if (path == "/sse") {
      content_type = "text/event-stream";
      payload = "event: message\ndata: {\"value\":1}\n\nevent: message\ndata: "
                "{\"value\":2}\n\ndata: [DONE]\n\n";
    } else if (path == "/v1/chat/completions" || path == "/chat/completions" ||
               path == "/api/v1/chat/completions") {
      auto model = j.is_object() ? j.value("model", std::string("fixture-model")) : "fixture-model";
      std::string answer = "Hello from fixture";
      if (j.is_object() && j.contains("response_format"))
        answer = "{\"ok\":true,\"answer\":42}";
      if (j.is_object() && j.value("stream", false)) {
        content_type = "text/event-stream";
        if (model == "tool-stream-fixture") {
          Json first = {{"model", model}, {"choices", Json::array()}};
          Json first_delta = {{"index", 0},
                              {"id", "call_1"},
                              {"type", "function"},
                              {"function", {{"name", "lookup"}, {"arguments", "{\"city\":\""}}}};
          first["choices"].push_back({{"delta", {{"tool_calls", Json::array({first_delta})}}}});
          Json second = {{"choices", Json::array()}};
          Json second_delta = {{"index", 0}, {"function", {{"arguments", "Paris\"}"}}}};
          second["choices"].push_back({{"delta", {{"tool_calls", Json::array({second_delta})}}}});
          Json usage = {{"choices", Json::array()},
                        {"usage", {{"prompt_tokens", 4}, {"completion_tokens", 3}}}};
          payload = "data: " + first.dump() + "\n\ndata: " + second.dump() +
                    "\n\ndata: " + usage.dump() + "\n\ndata: [DONE]\n\n";
        } else {
          payload = "data: " +
                    Json({{"model", model},
                          {"choices", Json::array({{{"delta", {{"content", answer}}}}})}})
                        .dump() +
                    "\n\ndata: " +
                    Json({{"choices", Json::array()},
                          {"usage", {{"prompt_tokens", 4}, {"completion_tokens", 3}}}})
                        .dump() +
                    "\n\ndata: [DONE]\n\n";
        }
      } else
        result = {
            {"id", "fixture-chat"},
            {"model", model},
            {"choices", Json::array({{{"message", {{"role", "assistant"}, {"content", answer}}},
                                      {"finish_reason", "stop"}}})},
            {"usage", {{"prompt_tokens", 4}, {"completion_tokens", 3}}}};
    } else if (path == "/api/chat" || path == "/chat") {
      auto model = j.is_object() ? j.value("model", std::string("fixture-model")) : "fixture-model";
      auto answer = j.is_object() && j.contains("format") ? "{\"ok\":true,\"answer\":42}"
                                                          : "Hello from fixture";
      if (j.is_object() && j.value("stream", false)) {
        content_type = "application/x-ndjson";
        payload =
            Json({{"model", model}, {"message", {{"content", answer}}}, {"done", false}}).dump() +
            "\n" +
            Json({{"model", model}, {"done", true}, {"prompt_eval_count", 4}, {"eval_count", 3}})
                .dump() +
            "\n";
      } else
        result = {{"model", model},
                  {"message", {{"role", "assistant"}, {"content", answer}}},
                  {"done", true},
                  {"prompt_eval_count", 4},
                  {"eval_count", 3}};
    } else if (path == "/users" || path.rfind("/users/", 0) == 0) {
      std::lock_guard<std::mutex> lock(mutex);
      if (path == "/users" && verb == "POST") {
        auto id = std::to_string(next_user++);
        users[id] = j.is_object() ? j : Json::object();
        users[id]["id"] = std::stoi(id);
        result = users[id];
        status = 201;
      } else if (path == "/users") {
        result = Json::array();
        for (auto &v : users)
          result.push_back(v);
      } else {
        auto id = path.substr(7);
        if (!users.contains(id)) {
          status = 404;
          result = {{"error", "not found"}};
        } else if (verb == "DELETE") {
          users.erase(id);
          result = {{"deleted", true}};
        } else if (verb == "PUT" || verb == "PATCH") {
          users[id].update(j.is_object() ? j : Json::object());
          result = users[id];
        } else
          result = users[id];
      }
    } else {
      result = {{"method", verb}, {"path", path}, {"args", args},       {"headers", headers},
                {"body", body},   {"json", j},    {"form", pairs(body)}};
    }
    if (payload.empty())
      payload = result.dump();
    std::string reason = status < 400 ? "OK" : "Error";
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                      "\r\nContent-Type: " + content_type +
                      "\r\nContent-Length: " + std::to_string(payload.size()) +
                      "\r\nConnection: close\r\n" + extra + "\r\n";
    if (verb != "HEAD")
      out += payload;
    (void)send_all(fd, out);
  }
};
Mock::Mock(Runtime *r) : runtime(r), state(std::make_unique<MockState>(r)) {}
Mock::~Mock() = default;
std::string Mock::url() const { return "http://127.0.0.1:" + std::to_string(state->port); }
void Mock::stop() { state->stop(); }
void Mock::serve(const std::string &ready_file, const std::string &stop_file, int64_t timeout_ms) {
  if (ready_file.empty() || stop_file.empty() || timeout_ms < 1 || timeout_ms > 120000)
    throw dslvm::Error("mock serve requires non-empty paths and timeout 1..120000 ms");
  if (ready_file == stop_file || state->stopping)
    throw dslvm::Error("mock serve paths must differ and server must be running");
  auto temporary = ready_file + ".tmp";
  runtime->write(temporary, url() + "\n");
  std::error_code ec;
  std::filesystem::rename(temporary, ready_file, ec);
  if (ec)
    throw dslvm::Error("mock serve cannot publish ready file: " + ec.message());
  auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!state->stopping && Clock::now() < deadline) {
    if (std::filesystem::exists(stop_file)) {
      stop();
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  stop();
  if (!std::filesystem::exists(stop_file))
    throw dslvm::Error("mock serve timed out waiting for stop file");
}
} // namespace lmcurl
