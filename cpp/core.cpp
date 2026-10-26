#include "core.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
namespace lmcurl {
std::string lower(std::string s) {
  for (auto &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string trim(std::string s) {
  auto a = s.find_first_not_of(" \r\n\t");
  if (a == std::string::npos)
    return "";
  return s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}
Runtime::Runtime() {
  for (const char *n : {"OPENROUTER_API_KEY", "DEEPSEEK_API_KEY", "OLLAMA_API_KEY",
                        "OPENAI_API_KEY", "GITHUB_TOKEN", "GH_TOKEN"})
    if (const char *value = std::getenv(n))
      secret(value);
}
Runtime::~Runtime() { objects.clear(); }
Json parse_json(const std::string &text, bool throwing) {
  if (text.size() > 8 * 1024 * 1024)
    throw dslvm::Error("JSON size limit exceeded");
  return Json::parse(
      text,
      [](int depth, Json::parse_event_t, Json &) {
        if (depth > 128)
          throw dslvm::Error("JSON nesting exceeds 128");
        return true;
      },
      throwing);
}
void Runtime::secret(const std::string &value) {
  if (value.empty() || value == "[REDACTED]")
    return;
  std::lock_guard<std::mutex> lock(secret_mutex);
  if (std::find(secrets.begin(), secrets.end(), value) == secrets.end()) {
    std::size_t bytes = value.size();
    for (const auto &v : secrets)
      bytes += v.size();
    if (secrets.size() >= 4096 || bytes > 1024 * 1024)
      throw dslvm::Error("secret registry limit exceeded");
    secrets.push_back(value);
  }
}
std::string Runtime::env_secret(const std::string &name) {
  if (name.empty() || name.size() > 128 ||
      name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") !=
          std::string::npos)
    throw dslvm::Error("invalid environment variable name");
  const char *v = std::getenv(name.c_str());
  std::string value = v ? v : "";
  secret(value);
  return value;
}
std::string Runtime::redact(std::string text) const {
  std::vector<std::string> values;
  {
    std::lock_guard<std::mutex> lock(secret_mutex);
    values = secrets;
  }
  std::sort(values.begin(), values.end(),
            [](const auto &a, const auto &b) { return a.size() > b.size(); });
  auto clean = [&](std::string value) {
    for (const auto &v : values) {
      std::size_t p = 0;
      while ((p = value.find(v, p)) != std::string::npos) {
        value.replace(p, v.size(), "[REDACTED]");
        p += 10;
      }
    }
    static const std::regex token(
        R"((sk-[A-Za-z0-9_-]{16,256}|(?:ghp_|github_pat_)[A-Za-z0-9_]{16,256}|Bearer\s+[A-Za-z0-9._~+/=-]{8,512}))",
        std::regex::icase);
    value = std::regex_replace(value, token, "[REDACTED]");
    static const std::regex credentials(R"((https?://)[^/\s:@]{1,128}:[^/\s@]{1,512}@)",
                                        std::regex::icase);
    return std::regex_replace(value, credentials, "$1[REDACTED]@");
  };
  auto sensitive = [](std::string k) {
    k = lower(k);
    return k == "authorization" || k == "proxy-authorization" || k == "x-api-key" ||
           k == "api-key" || k == "api_key" || k == "token" || k == "access_token" ||
           k == "refresh_token" || k == "password" || k == "secret" || k == "client_secret" ||
           k == "private_key" || k == "passphrase";
  };
  auto first = text.find_first_not_of(" \r\n\t");
  if (first != std::string::npos && (text[first] == '{' || text[first] == '[') &&
      text.size() <= 8 * 1024 * 1024) {
    auto j = parse_json(text, false);
    if (!j.is_discarded()) {
      std::function<void(Json &)> walk = [&](Json &v) {
        if (v.is_object()) {
          Json safe = Json::object();
          for (auto it = v.begin(); it != v.end(); ++it) {
            auto child = it.value();
            if (sensitive(it.key()))
              child = "[REDACTED]";
            else
              walk(child);
            safe[clean(it.key())] = std::move(child);
          }
          v = std::move(safe);
        } else if (v.is_array()) {
          for (auto &child : v)
            walk(child);
        } else if (v.is_string())
          v = clean(v.get<std::string>());
        else if (std::find(values.begin(), values.end(), v.dump()) != values.end())
          v = "[REDACTED]";
      };
      walk(j);
      return j.dump();
    }
  }
  text = clean(text);
  static const std::regex auth(
      R"((Authorization|Proxy-Authorization|X-Api-Key|Api-Key)(\s*[=:]\s*)[^\r\n]{1,8192})",
      std::regex::icase);
  return std::regex_replace(text, auth, "$1$2[REDACTED]");
}
void Runtime::discover_secrets(const Json &value) {
  if (value.is_object())
    for (auto it = value.begin(); it != value.end(); ++it) {
      auto k = lower(it.key());
      bool sensitive = k == "token" || k == "access_token" || k == "refresh_token" ||
                       k == "session_token" || k == "password" || k == "passphrase" ||
                       k == "secret" || k == "client_secret" || k == "private_key" ||
                       k == "api_key" || k == "apikey" || k == "authorization" ||
                       k == "proxy-authorization";
      if (sensitive && it.value().is_string())
        secret(it.value().get<std::string>());
      else
        discover_secrets(it.value());
    }
  else if (value.is_array())
    for (const auto &v : value)
      discover_secrets(v);
}
void Runtime::write(const std::string &path, const std::string &text) const {
  if (path.empty() || path.find('\0') != std::string::npos)
    throw dslvm::Error("invalid output path");
  auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent);
  std::ofstream out(path, std::ios::binary);
  if (!out)
    throw dslvm::Error("cannot open output file");
  auto safe = redact(text);
  out.write(safe.data(), static_cast<std::streamsize>(safe.size()));
  out.close();
  if (!out)
    throw dslvm::Error("output write failed");
}
std::string read_file(const std::string &path, std::size_t limit) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw dslvm::Error("cannot read input file");
  std::string out;
  char buf[8192];
  while (in) {
    in.read(buf, sizeof(buf));
    auto n = static_cast<std::size_t>(in.gcount());
    if (n > limit - out.size())
      throw dslvm::Error("input file size limit exceeded");
    membudget::require(n);
    out.append(buf, n);
  }
  if (!in.eof())
    throw dslvm::Error("input read failed");
  return out;
}
std::string csv_cell(const std::string &x) {
  std::string s = "\"";
  for (char c : x) {
    if (c == '\"')
      s += '\"';
    s += c;
  }
  return s + '\"';
}
std::string html_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '\"':
      out += "&quot;";
      break;
    case '\'':
      out += "&#39;";
      break;
    default:
      out += c;
    }
  }
  return out;
}
std::string sha256(const std::string &text) {
  unsigned char out[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(text.data()), text.size(), out);
  std::ostringstream s;
  for (auto c : out)
    s << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  return s.str();
}
void method(dslvm::Registry &r, Runtime &rt, const char *type, const char *name,
            std::vector<std::string> params, const char *returns, dslvm::Method fn) {
  std::string who = std::string(type) + "." + name;
  r.method(type, name, params, returns, [&rt, params, who, fn](void *self, Args &a) {
    if (!self || a.size() != params.size())
      throw dslvm::Error(who + ": invalid arguments");
    for (std::size_t i = 0; i < a.size(); ++i) {
      auto &p = params[i];
      auto &v = a[i];
      bool ok = p == "string"  ? v.kind == Value::STR
                : p == "int"   ? v.kind == Value::INT
                : p == "float" ? v.kind == Value::FLOAT
                : p == "bool"  ? v.kind == Value::BOOL
                               : v.kind == Value::OBJ && v.obj && v.type == p;
      if (!ok)
        throw dslvm::Error(who + ": argument type mismatch");
    }
    try {
      return fn(self, a);
    } catch (const dslvm::Error &e) {
      throw dslvm::Error(rt.redact(e.what()));
    } catch (const std::exception &e) {
      throw dslvm::Error(rt.redact(e.what()));
    }
  });
}
void register_safe_std(dslvm::Registry &r, Runtime &rt) {
  for (const auto *name : {"print", "println"})
    r.function(name, {"string"}, "void", [&rt](Args &a) {
      std::cout << rt.redact(a[0].s) << '\n';
      return Value::none();
    });
  for (const auto *name : {"file_write", "file_append"}) {
    auto sig = std::string("fn ") + name + "(string, string) -> void";
    // Middleware preserves the pinned standard declaration and delegates its path/error handling.
    auto *entry = const_cast<dslvm::FunctionEntry *>(r.function_by_signature(sig));
    if (!entry)
      throw dslvm::Error("pinned SDK file callback unavailable");
    auto original = entry->fn;
    entry->fn = [&rt, original](Args &a) {
      Args safe = a;
      safe[1].s = rt.redact(safe[1].s);
      return original(safe);
    };
  }
  r.function("assert", {"bool"}, "void", [](Args &a) {
    if (!a[0].b)
      throw dslvm::Error("assertion failed");
    return Value::none();
  });
  r.function("assert_eq", {"int", "int"}, "void", [](Args &a) {
    if (a[0].i != a[1].i)
      throw dslvm::Error("assert_eq(int) failed");
    return Value::none();
  });
  r.function("assert_eq", {"string", "string"}, "void", [](Args &a) {
    if (a[0].s != a[1].s)
      throw dslvm::Error("assert_eq(string) failed");
    return Value::none();
  });
  r.function("assert_eq", {"bool", "bool"}, "void", [](Args &a) {
    if (a[0].b != a[1].b)
      throw dslvm::Error("assert_eq(bool) failed");
    return Value::none();
  });
  r.function("assert_close", {"float", "float", "float"}, "void", [](Args &a) {
    if (!(std::fabs(a[0].f - a[1].f) <= a[2].f))
      throw dslvm::Error("assert_close failed");
    return Value::none();
  });
}
} // namespace lmcurl
