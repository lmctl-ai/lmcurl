#include "core.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <condition_variable>
#include <thread>
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
    if (path.rfind("/bytes/", 0) == 0) {
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
      bool ok = headers.value("cookie", std::string()).find("session=fixture-session") !=
                    std::string::npos ||
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
                              {"function", {{"name", "lookup"},
                                             {"arguments", "{\"city\":\""}}}};
          first["choices"].push_back({{"delta", {{"tool_calls", Json::array({first_delta})}}}});
          Json second = {{"choices", Json::array()}};
          Json second_delta = {{"index", 0}, {"function", {{"arguments", "Paris\"}"}}}};
          second["choices"].push_back({{"delta", {{"tool_calls", Json::array({second_delta})}}}});
          Json usage = {{"choices", Json::array()},
                        {"usage", {{"prompt_tokens", 4}, {"completion_tokens", 3}}}};
          payload = "data: " + first.dump() + "\n\ndata: " + second.dump() +
                    "\n\ndata: " + usage.dump() + "\n\ndata: [DONE]\n\n";
        } else {
          payload =
              "data: " +
              Json({{"model", model}, {"choices", Json::array({{{"delta", {{"content", answer}}}}})}})
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
} // namespace lmcurl
