#pragma once
#include "dslvm.hpp"
#include "memory_budget.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace lmcurl {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Args = dslvm::Args;
using Value = dslvm::Value;
struct Runtime {
  std::vector<std::shared_ptr<void>> objects;
  std::vector<std::string> secrets;
  mutable std::mutex object_mutex, secret_mutex, cost_mutex;
  double llm_spent = 0, llm_reserved = 0, llm_cap = 0.02;
  Json llm_calls = Json::array();
  std::atomic<bool> failed{false};
  Runtime();
  ~Runtime();
  template <class T, class... A> T *hold(A &&...a) {
    auto ptr = std::shared_ptr<T>(new T{std::forward<A>(a)...});
    std::lock_guard<std::mutex> lock(object_mutex);
    if (objects.size() >= 100000)
      throw dslvm::Error("host object limit exceeded");
    objects.push_back(ptr);
    return ptr.get();
  }
  void secret(const std::string &value);
  std::string env_secret(const std::string &name);
  std::string redact(std::string text) const;
  void discover_secrets(const Json &value);
  void write(const std::string &path, const std::string &text) const;
};
void method(dslvm::Registry &, Runtime &, const char *type, const char *name,
            std::vector<std::string> params, const char *returns, dslvm::Method);
Json parse_json(const std::string &, bool throw_on_error = true);
std::string lower(std::string);
std::string trim(std::string);
std::string read_file(const std::string &path, std::size_t max_bytes = 8 * 1024 * 1024);
std::string csv_cell(const std::string &);
std::string html_escape(const std::string &);
std::string sha256(const std::string &);
struct Reservation {
  std::size_t bytes;
  explicit Reservation(std::size_t n) : bytes(n) { membudget::charge(n); }
  ~Reservation() { membudget::release(bytes); }
  Reservation(const Reservation &) = delete;
};
struct Response {
  Runtime *runtime = nullptr;
  int64_t status = 0, size = 0, attempts = 0;
  std::string body, url, error, certificate;
  std::map<std::string, std::string> headers;
  double total_ms = 0, dns_ms = 0, connect_ms = 0, tls_ms = 0, ttfb_ms = 0, first_data_ms = 0;
};
struct CookieState;
struct Request;
struct Session {
  Runtime *runtime;
  std::string base;
  std::map<std::string, std::string> headers, variables;
  std::shared_ptr<CookieState> state;
  mutable std::mutex mutex;
  Session(Runtime *runtime, std::string base);
  ~Session();
  Response perform(const Request &) const;
  std::string expand(const std::string &) const;
};
struct Part {
  std::string name, value, filename;
  bool file = false;
};
struct Request {
  Session *session;
  std::string verb, url, body, proxy, cacert, client_cert, client_key, resolve;
  std::map<std::string, std::string> headers, form;
  std::vector<Part> parts;
  std::string auth_user, auth_password, auth_kind;
  int64_t timeout_ms = 10000, retries = 0, retry_ms = 20, max_bytes = 4 * 1024 * 1024;
  bool follow = true, insecure = false, http2 = false, stream = false, llm_budgeted = false;
  bool literal = false;
  int redirects_left = 5;
  Clock::time_point deadline{};
  Request(Session *s, std::string method, std::string target)
      : session(s), verb(std::move(method)), url(std::move(target)) {}
  Response send() const { return session->perform(*this); }
};
struct MockState;
struct Mock {
  Runtime *runtime;
  std::unique_ptr<MockState> state;
  explicit Mock(Runtime *runtime);
  ~Mock();
  std::string url() const;
  void stop();
  void serve(const std::string &ready_file, const std::string &stop_file, int64_t timeout_ms);
};
void transport_init();
void register_http(dslvm::Registry &, Runtime &);
void register_framework(dslvm::Registry &, Runtime &);
void register_llm(dslvm::Registry &, Runtime &);
void register_safe_std(dslvm::Registry &, Runtime &);
} // namespace lmcurl
