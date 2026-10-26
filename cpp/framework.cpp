#include "core.hpp"
#include "query.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <sstream>
#include <thread>
namespace lmcurl {
namespace {
struct Expect {
  std::string kind, path, value;
  int64_t low = 0, high = 0;
};
struct Capture {
  std::string name, path;
  bool secret = false;
};
struct Test {
  Runtime *rt;
  std::string name;
  Request *request;
  std::vector<Expect> expects;
  std::vector<Capture> captures;
  std::vector<std::string> tags;
  int64_t retries = 0, interval = 0, timeout = 10000;
  Test(Runtime *r, std::string n, Request *q) : rt(r), name(std::move(n)), request(q) {}
};
struct Case {
  std::string name;
  bool passed = false;
  std::vector<std::string> errors;
  int64_t assertions = 0, attempts = 0;
  double ms = 0;
};
struct Report {
  Runtime *rt;
  std::string name;
  std::vector<Case> cases;
  int64_t skipped = 0;
  Report(Runtime *r, std::string n) : rt(r), name(std::move(n)) {}
  int64_t failures() const {
    return std::count_if(cases.begin(), cases.end(), [](const auto &c) { return !c.passed; });
  }
};
struct Suite {
  Runtime *rt;
  std::string name, filter;
  std::vector<Test *> tests;
  std::vector<Request *> before, after;
  std::vector<std::map<std::string, std::string>> rows;
  bool fail_fast = false, parallel = false, has_data = false;
  Suite(Runtime *r, std::string n) : rt(r), name(std::move(n)) {}
};
struct Load {
  int64_t requests = 0, errors = 0;
  double p50 = 0, p95 = 0, p99 = 0, rate = 0;
};
std::string scalar(const Json &j) { return j.is_string() ? j.get<std::string>() : j.dump(); }
std::vector<std::vector<std::string>> csv(const std::string &text) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string value;
  bool quoted = false;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    char c = i < text.size() ? text[i] : '\n';
    if (c == '\"') {
      if (quoted && i + 1 < text.size() && text[i + 1] == '\"') {
        value += '\"';
        ++i;
      } else
        quoted = !quoted;
    } else if (!quoted && (c == ',' || c == '\n')) {
      row.push_back(value);
      value.clear();
      if (c == '\n') {
        if (!(row.size() == 1 && row[0].empty()))
          rows.push_back(row);
        row.clear();
        if (rows.size() > 1001)
          throw dslvm::Error("dataset row limit is 1000");
      }
    } else if (c != '\r')
      value += c;
  }
  if (quoted)
    throw dslvm::Error("unterminated CSV quote");
  return rows;
}
void dataset(Suite &s, const Json &j) {
  if (!j.is_array() || j.size() > 1000)
    throw dslvm::Error("dataset must be an array of at most 1000 objects");
  s.has_data = true;
  s.rows.clear();
  for (const auto &row : j) {
    if (!row.is_object())
      throw dslvm::Error("dataset rows must be objects");
    std::map<std::string, std::string> r;
    for (auto it = row.begin(); it != row.end(); ++it)
      r[it.key()] = scalar(it.value());
    s.rows.push_back(std::move(r));
  }
}
Case execute(Test &t) {
  Case c;
  c.name = t.name;
  auto start = Clock::now(), deadline = start + std::chrono::milliseconds(t.timeout);
  for (int64_t attempt = 0; attempt <= t.retries; ++attempt) {
    c.errors.clear();
    c.assertions = 0;
    ++c.attempts;
    auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left < 1) {
      c.errors.push_back("test timeout");
      break;
    }
    Request req = *t.request;
    req.retries = 0;
    req.timeout_ms = std::min<int64_t>(req.timeout_ms, left);
    Response response;
    try {
      response = req.send();
    } catch (const membudget::Exceeded &) {
      throw;
    } catch (const std::bad_alloc &) {
      throw;
    } catch (const std::exception &e) {
      c.errors.push_back(e.what());
    }
    auto check = [&](bool ok, const std::string &message) {
      ++c.assertions;
      if (!ok)
        c.errors.push_back(message);
    };
    check(response.error.empty(), "transport error: " + response.error);
    if (t.expects.empty())
      check(response.status >= 200 && response.status < 400, "HTTP status is not successful");
    for (const auto &e : t.expects) {
      if (e.kind == "status")
        check(response.status == e.low, "status mismatch: " + std::to_string(response.status));
      else if (e.kind == "range")
        check(response.status >= e.low && response.status <= e.high,
              "status outside expected range");
      else if (e.kind == "body")
        check(response.body.find(t.request->session->expand(e.value)) != std::string::npos,
              "body did not contain expected text");
      else if (e.kind == "exact")
        check(response.body == t.request->session->expand(e.value), "body differs");
      else if (e.kind == "schema") {
        try {
          check(schema_valid(parse_json(response.body), e.value), "JSON schema mismatch");
        } catch (const std::exception &) {
          check(false, "invalid JSON/schema");
        }
      } else if (e.kind == "xpath") {
        bool found = false;
        auto value = xpath_value(response.body, e.path, false, found);
        check(found && value == e.value, "XPath mismatch: " + e.path);
      } else {
        bool found = false;
        try {
          auto value = select_json(parse_json(response.body), e.path, found);
          check(found && scalar(value) == t.request->session->expand(e.value),
                "JSON query mismatch: " + e.path);
        } catch (const std::exception &) {
          check(false, "JSON query failed: " + e.path);
        }
      }
    }
    if (c.errors.empty()) {
      for (const auto &cap : t.captures) {
        bool found = false;
        auto value = select_json(parse_json(response.body), cap.path, found);
        if (!found) {
          c.errors.push_back("capture missing: " + cap.path);
          break;
        }
        auto v = scalar(value);
        if (cap.secret)
          t.rt->secret(v);
        {
          std::lock_guard<std::mutex> lock(t.request->session->mutex);
          t.request->session->variables[cap.name] = v;
        }
      }
    }
    if (c.errors.empty())
      break;
    if (attempt < t.retries) {
      auto delay = std::min<int64_t>(
          t.interval, std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                                               deadline - Clock::now())
                                               .count()));
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
  }
  c.passed = c.errors.empty();
  c.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  for (auto &e : c.errors)
    e = t.rt->redact(e);
  return c;
}
Report *run(Suite &s) {
  auto *out = s.rt->hold<Report>(s.rt, s.name);
  if (s.tests.size() > 1000)
    throw dslvm::Error("suite test limit is 1000");
  if (s.parallel && (s.has_data || std::any_of(s.tests.begin(), s.tests.end(),
                                               [](auto *t) { return !t->captures.empty(); })))
    throw dslvm::Error("parallel suites require independent tests without datasets/captures");
  std::exception_ptr fatal;
  auto hook = [&](Request *q, const char *name) {
    Case c;
    c.name = name;
    c.assertions = 1;
    try {
      auto r = q->send();
      c.passed = r.error.empty() && r.status >= 200 && r.status < 400;
      if (!c.passed)
        c.errors.push_back("hook request failed");
    } catch (const std::exception &e) {
      c.errors.push_back(s.rt->redact(e.what()));
    }
    out->cases.push_back(c);
    return c.passed;
  };
  try {
    bool ready = true;
    for (auto *q : s.before)
      ready = hook(q, "setup") && ready;
    if (ready) {
      auto execute_row = [&](const std::map<std::string, std::string> *row) {
        std::vector<Test *> selected;
        for (auto *t : s.tests) {
          if (!s.filter.empty() &&
              std::find(t->tags.begin(), t->tags.end(), s.filter) == t->tags.end()) {
            ++out->skipped;
            continue;
          }
          if (row) {
            std::lock_guard<std::mutex> lock(t->request->session->mutex);
            for (const auto &kv : *row)
              t->request->session->variables[kv.first] = kv.second;
          }
          selected.push_back(t);
        }
        if (s.parallel) {
          std::atomic<std::size_t> next{0};
          std::atomic<bool> stop{false};
          std::mutex mutex;
          std::vector<std::thread> workers;
          auto work = [&] {
            while (!stop) {
              auto i = next.fetch_add(1);
              if (i >= selected.size())
                break;
              try {
                auto c = execute(*selected[i]);
                bool failed = !c.passed;
                {
                  std::lock_guard<std::mutex> lock(mutex);
                  out->cases.push_back(std::move(c));
                }
                if (failed && s.fail_fast)
                  stop = true;
              } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                if (!fatal)
                  fatal = std::current_exception();
                stop = true;
              }
            }
          };
          try {
            for (std::size_t i = 0; i < std::min<std::size_t>(8, selected.size()); ++i)
              workers.emplace_back(work);
          } catch (...) {
            stop = true;
            for (auto &t : workers)
              t.join();
            throw;
          }
          for (auto &t : workers)
            t.join();
        } else
          for (auto *t : selected) {
            auto c = execute(*t);
            bool failed = !c.passed;
            out->cases.push_back(std::move(c));
            if (failed && s.fail_fast)
              break;
          }
      };
      if (!s.has_data)
        execute_row(nullptr);
      else
        for (const auto &row : s.rows) {
          execute_row(&row);
          if (s.fail_fast && out->failures())
            break;
        }
    }
  } catch (...) {
    fatal = std::current_exception();
  }
  for (auto *q : s.after)
    hook(q, "teardown");
  if (fatal)
    std::rethrow_exception(fatal);
  return out;
}
Json report_json(const Report &r) {
  Json cases = Json::array();
  for (const auto &c : r.cases)
    cases.push_back({{"name", c.name},
                     {"passed", c.passed},
                     {"assertions", c.assertions},
                     {"attempts", c.attempts},
                     {"latency_ms", c.ms},
                     {"errors", c.errors}});
  return {{"name", r.name},
          {"passed", r.cases.size() - r.failures()},
          {"failed", r.failures()},
          {"skipped", r.skipped},
          {"cases", cases}};
}
} // namespace
void register_framework(dslvm::Registry &r, Runtime &rt) {
  register_queries(r, rt);
  method(r, rt, "Curl", "suite", {"string"}, "Suite",
         [&rt](void *, Args &a) { return Value::object("Suite", rt.hold<Suite>(&rt, a[0].s)); });
  method(r, rt, "Suite", "test_request", {"string", "Request"}, "Test", [&rt](void *p, Args &a) {
    auto &s = *static_cast<Suite *>(p);
    auto *t = rt.hold<Test>(&rt, a[0].s, static_cast<Request *>(a[1].obj));
    s.tests.push_back(t);
    return Value::object("Test", t);
  });
  for (const char *name : {"before", "after"})
    method(r, rt, "Suite", name, {"Request"}, "Suite", [name](void *p, Args &a) {
      auto &s = *static_cast<Suite *>(p);
      (std::string(name) == "before" ? s.before : s.after)
          .push_back(static_cast<Request *>(a[0].obj));
      return Value::object("Suite", p);
    });
  method(r, rt, "Suite", "filter", {"string"}, "Suite", [](void *p, Args &a) {
    static_cast<Suite *>(p)->filter = a[0].s;
    return Value::object("Suite", p);
  });
  for (const char *name : {"parallel", "fail_fast"})
    method(r, rt, "Suite", name, {"bool"}, "Suite", [name](void *p, Args &a) {
      auto &s = *static_cast<Suite *>(p);
      (std::string(name) == "parallel" ? s.parallel : s.fail_fast) = a[0].b;
      return Value::object("Suite", p);
    });
  method(r, rt, "Suite", "data_json", {"string"}, "Suite", [](void *p, Args &a) {
    dataset(*static_cast<Suite *>(p), parse_json(read_file(a[0].s)));
    return Value::object("Suite", p);
  });
  method(r, rt, "Suite", "data_csv", {"string"}, "Suite", [](void *p, Args &a) {
    auto rows = csv(read_file(a[0].s));
    if (rows.empty())
      throw dslvm::Error("CSV dataset has no header");
    Json j = Json::array();
    for (std::size_t i = 1; i < rows.size(); ++i) {
      if (rows[i].size() != rows[0].size())
        throw dslvm::Error("CSV column count mismatch");
      Json row = Json::object();
      for (std::size_t c = 0; c < rows[0].size(); ++c)
        row[rows[0][c]] = rows[i][c];
      j.push_back(row);
    }
    dataset(*static_cast<Suite *>(p), j);
    return Value::object("Suite", p);
  });
  method(r, rt, "Test", "expect_status", {"int"}, "Test", [](void *p, Args &a) {
    static_cast<Test *>(p)->expects.push_back({"status", "", "", a[0].i, a[0].i});
    return Value::object("Test", p);
  });
  method(r, rt, "Test", "expect_status_in", {"int", "int"}, "Test", [](void *p, Args &a) {
    static_cast<Test *>(p)->expects.push_back({"range", "", "", a[0].i, a[1].i});
    return Value::object("Test", p);
  });
  for (const char *name : {"expect_body", "expect_exact", "expect_schema"})
    method(r, rt, "Test", name, {"string"}, "Test", [name](void *p, Args &a) {
      std::string n = name;
      static_cast<Test *>(p)->expects.push_back({n == "expect_body"    ? "body"
                                                 : n == "expect_exact" ? "exact"
                                                                       : "schema",
                                                 "", a[0].s});
      return Value::object("Test", p);
    });
  for (const char *name : {"expect_json", "expect_xpath"})
    method(r, rt, "Test", name, {"string", "string"}, "Test", [name](void *p, Args &a) {
      static_cast<Test *>(p)->expects.push_back(
          {std::string(name) == "expect_json" ? "json" : "xpath", a[0].s, a[1].s});
      return Value::object("Test", p);
    });
  for (const char *name : {"capture", "capture_secret"})
    method(r, rt, "Test", name, {"string", "string"}, "Test", [name](void *p, Args &a) {
      static_cast<Test *>(p)->captures.push_back(
          {a[0].s, a[1].s, std::string(name) == "capture_secret"});
      return Value::object("Test", p);
    });
  method(r, rt, "Test", "tag", {"string"}, "Test", [](void *p, Args &a) {
    static_cast<Test *>(p)->tags.push_back(a[0].s);
    return Value::object("Test", p);
  });
  method(r, rt, "Test", "timeout", {"int"}, "Test", [](void *p, Args &a) {
    if (a[0].i < 1 || a[0].i > 30000)
      throw dslvm::Error("test timeout must be 1..30000 ms");
    static_cast<Test *>(p)->timeout = a[0].i;
    return Value::object("Test", p);
  });
  method(r, rt, "Test", "retry", {"int", "int"}, "Test", [](void *p, Args &a) {
    if (a[0].i < 0 || a[0].i > 5 || a[1].i < 0 || a[1].i > 2000)
      throw dslvm::Error("test retry bounds exceeded");
    auto &t = *static_cast<Test *>(p);
    t.retries = a[0].i;
    t.interval = a[1].i;
    return Value::object("Test", p);
  });
  method(r, rt, "Test", "poll_json", {"string", "string", "int", "int"}, "Test",
         [](void *p, Args &a) {
           if (a[2].i < 0 || a[2].i > 5 || a[3].i < 0 || a[3].i > 2000)
             throw dslvm::Error("poll bounds exceeded");
           auto &t = *static_cast<Test *>(p);
           t.expects.push_back({"json", a[0].s, a[1].s});
           t.retries = a[2].i;
           t.interval = a[3].i;
           return Value::object("Test", p);
         });
  method(r, rt, "Suite", "report", {}, "Report",
         [](void *p, Args &) { return Value::object("Report", run(*static_cast<Suite *>(p))); });
  method(r, rt, "Report", "passed", {}, "int", [](void *p, Args &) {
    auto &r = *static_cast<Report *>(p);
    return Value::integer(static_cast<int64_t>(r.cases.size()) - r.failures());
  });
  method(r, rt, "Report", "failed", {}, "int",
         [](void *p, Args &) { return Value::integer(static_cast<Report *>(p)->failures()); });
  method(r, rt, "Report", "skipped", {}, "int",
         [](void *p, Args &) { return Value::integer(static_cast<Report *>(p)->skipped); });
  method(r, rt, "Report", "assert_all", {}, "bool", [](void *p, Args &) {
    auto &r = *static_cast<Report *>(p);
    if (r.failures())
      r.rt->failed = true;
    return Value::boolean(r.failures() == 0);
  });
  method(r, rt, "Report", "json", {}, "string", [](void *p, Args &) {
    auto &r = *static_cast<Report *>(p);
    return Value::string(r.rt->redact(report_json(r).dump()));
  });
  method(r, rt, "Report", "save", {"string"}, "void", [](void *p, Args &a) {
    auto &r = *static_cast<Report *>(p);
    r.rt->write(a[0].s, report_json(r).dump(2) + "\n");
    return Value::none();
  });
  method(r, rt, "Report", "console", {}, "string", [](void *p, Args &) {
    auto &r = *static_cast<Report *>(p);
    return Value::string("passed=" + std::to_string(r.cases.size() - r.failures()) + " failed=" +
                         std::to_string(r.failures()) + " skipped=" + std::to_string(r.skipped));
  });
  method(r, rt, "Report", "tap", {}, "string", [](void *p, Args &) {
    auto &r = *static_cast<Report *>(p);
    std::string out = "TAP version 13\n1.." + std::to_string(r.cases.size()) + "\n";
    std::size_t i = 0;
    for (const auto &c : r.cases)
      out += (c.passed ? "ok " : "not ok ") + std::to_string(++i) + " - " + r.rt->redact(c.name) +
             "\n";
    return Value::string(out);
  });
  method(r, rt, "Report", "junit", {"string"}, "void", [](void *p, Args &a) {
    auto &r = *static_cast<Report *>(p);
    std::string out = "<?xml version=\"1.0\"?><testsuite name=\"" + html_escape(r.name) +
                      "\" tests=\"" + std::to_string(r.cases.size()) + "\" failures=\"" +
                      std::to_string(r.failures()) + "\">";
    for (const auto &c : r.cases) {
      out += "<testcase name=\"" + html_escape(c.name) + "\" time=\"" +
             std::to_string(c.ms / 1000) + "\">";
      for (const auto &e : c.errors)
        out += "<failure message=\"" + html_escape(e) + "\"/>";
      out += "</testcase>";
    }
    r.rt->write(a[0].s, out + "</testsuite>\n");
    return Value::none();
  });
  method(r, rt, "Report", "html", {"string"}, "void", [](void *p, Args &a) {
    auto &r = *static_cast<Report *>(p);
    std::string out = "<!doctype html><meta charset=utf-8><title>lmcurl report</title><h1>" +
                      html_escape(r.name) +
                      "</h1><table><tr><th>Test</th><th>Result</th><th>ms</th></tr>";
    for (const auto &c : r.cases)
      out += "<tr><td>" + html_escape(c.name) + "</td><td>" + (c.passed ? "PASS" : "FAIL") +
             "</td><td>" + std::to_string(c.ms) + "</td></tr>";
    r.rt->write(a[0].s, out + "</table>\n");
    return Value::none();
  });
  method(r, rt, "Report", "csv", {"string"}, "void", [](void *p, Args &a) {
    auto &r = *static_cast<Report *>(p);
    std::string out = "test,passed,assertions,attempts,latency_ms\n";
    for (const auto &c : r.cases)
      out += csv_cell(c.name) + "," + (c.passed ? "1" : "0") + "," + std::to_string(c.assertions) +
             "," + std::to_string(c.attempts) + "," + std::to_string(c.ms) + "\n";
    r.rt->write(a[0].s, out);
    return Value::none();
  });
  method(r, rt, "Curl", "load", {"Request", "int", "float", "int"}, "LoadResult",
         [&rt](void *, Args &a) {
           if (a[1].i < 1 || a[1].i > 32 || !std::isfinite(a[2].f) || a[2].f <= 0 || a[2].f > 30 ||
               a[3].i < 1 || a[3].i > 10000)
             throw dslvm::Error("load limits: 1..32 users, (0,30] seconds, 1..10000 requests");
           auto *out = rt.hold<Load>();
           auto *request = static_cast<Request *>(a[0].obj);
           auto end = Clock::now() + std::chrono::duration<double>(a[2].f);
           std::atomic<int64_t> next{0};
           std::atomic<bool> stop{false};
           std::mutex mutex;
           std::vector<double> samples;
           std::exception_ptr fatal;
           auto work = [&] {
             while (!stop && Clock::now() < end) {
               if (next.fetch_add(1) >= a[3].i)
                 break;
               auto start = Clock::now();
               bool error = false;
               try {
                 Request q = *request;
                 q.retries = 0;
                 q.timeout_ms = std::min<int64_t>(
                     q.timeout_ms,
                     std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(
                                              end - Clock::now())
                                              .count()));
                 auto r = q.send();
                 error = !r.error.empty() || r.status >= 400;
               } catch (const std::bad_alloc &) {
                 std::lock_guard<std::mutex> lock(mutex);
                 fatal = std::current_exception();
                 stop = true;
                 break;
               } catch (...) {
                 error = true;
               }
               std::lock_guard<std::mutex> lock(mutex);
               ++out->requests;
               if (error)
                 ++out->errors;
               samples.push_back(
                   std::chrono::duration<double, std::milli>(Clock::now() - start).count());
             }
           };
           std::vector<std::thread> workers;
           try {
             for (int64_t i = 0; i < a[1].i; ++i)
               workers.emplace_back(work);
           } catch (...) {
             stop = true;
             for (auto &t : workers)
               t.join();
             throw;
           }
           for (auto &t : workers)
             t.join();
           if (fatal)
             std::rethrow_exception(fatal);
           std::sort(samples.begin(), samples.end());
           if (!samples.empty()) {
             auto pct = [&](double p) {
               return samples[std::min(samples.size() - 1, static_cast<std::size_t>(
                                                               std::ceil(p * samples.size()) - 1))];
             };
             out->p50 = pct(.5);
             out->p95 = pct(.95);
             out->p99 = pct(.99);
             out->rate = double(out->errors) / out->requests;
           }
           return Value::object("LoadResult", out);
         });
  method(r, rt, "LoadResult", "requests", {}, "int",
         [](void *p, Args &) { return Value::integer(static_cast<Load *>(p)->requests); });
  for (const char *n : {"p50_ms", "p95_ms", "p99_ms", "error_rate"})
    method(r, rt, "LoadResult", n, {}, "float", [n](void *p, Args &) {
      auto &r = *static_cast<Load *>(p);
      return Value::real(std::string(n) == "p50_ms"   ? r.p50
                         : std::string(n) == "p95_ms" ? r.p95
                         : std::string(n) == "p99_ms" ? r.p99
                                                      : r.rate);
    });
  method(r, rt, "LoadResult", "threshold", {"float", "float"}, "bool", [](void *p, Args &a) {
    auto &r = *static_cast<Load *>(p);
    return Value::boolean(r.requests > 0 && r.p95 <= a[0].f && r.rate <= a[1].f);
  });
}
} // namespace lmcurl
