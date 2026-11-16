#include "core.hpp"
#include "schema_embedded.hpp"
#include "notices_embedded.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#ifndef LMCURL_VERSION
#define LMCURL_VERSION "0.1.4"
#endif
namespace {
int usage_error(const std::string &message) {
  return dslvm::failure_frame("cli", dslvm::Error("CLI_USAGE", message));
}
// Redact one frame payload field by field: Runtime::redact would sort the top-level keys of a JSON
// document, and the client requires a dry reply to end with its dry marker.
std::string redact_payload(const lmcurl::Runtime &rt, const std::string &text) {
  using Ordered = nlohmann::ordered_json;
  auto payload = Ordered::parse(text, nullptr, false);
  if (payload.is_discarded() || !payload.is_object())
    return rt.redact(text);
  for (auto &field : payload.items()) {
    auto &value = field.value();
    if (value.is_string()) {
      value = rt.redact(value.get<std::string>());
    } else if (value.is_structured()) {
      auto safe = Ordered::parse(rt.redact(value.dump()), nullptr, false);
      value = safe.is_discarded() ? Ordered("[REDACTED]") : std::move(safe);
    }
  }
  return payload.dump();
}
// The frame is one more host output: a program's result or failure text gets the same redaction as
// everything else lmcurl prints.
int emit_frame(const lmcurl::Runtime &rt, dslvm::InvocationResults &results) {
  static const std::string begin = "LMCODE_RETURN_BEGIN\n", end = "\nLMCODE_RETURN_END\n";
  std::ostringstream buffer;
  int code = results.write_frame(buffer);
  std::string frame = buffer.str();
  if (frame.size() > begin.size() + end.size() && frame.compare(0, begin.size(), begin) == 0 &&
      frame.compare(frame.size() - end.size(), end.size(), end) == 0)
    frame = begin + redact_payload(rt, frame.substr(begin.size(), frame.size() - begin.size() - end.size())) + end;
  else
    frame = rt.redact(frame);
  std::cout << frame << std::flush;
  if (!std::cout) {
    std::cerr << "lmcode invocation: cannot write envelope\n";
    return 4;
  }
  return code;
}

bool is_url_arg(const std::string &s) {
  return s.rfind("ldap://", 0) == 0 || s.rfind("ldaps://", 0) == 0 ||
         s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

std::string pct_encode(const std::string &in) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '=' || c == ',') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0xf]);
      out.push_back(hex[c & 0xf]);
    }
  }
  return out;
}

int run_cli_ldap_subcommand(lmcurl::Runtime &rt, int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: lmcurl ldap <ping|bind|whoami|search|get|add|modify|compare|del> [options]\n";
    return 2;
  }
  std::string sub = argv[2];
  std::string host = "127.0.0.1";
  int port = 389;
  std::string url_opt;
  std::string base_dn;
  std::string filter = "(objectClass=*)";
  std::string scope = "sub";
  std::string attrs;
  std::string dn_opt;
  std::string pw_opt;
  std::string bind_dn;
  std::string bind_pw;
  std::vector<std::string> pos;

  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc)
        throw dslvm::Error("missing value for " + a);
      return argv[++i];
    };
    if (a == "--host")
      host = next();
    else if (a == "--port")
      port = std::atoi(next().c_str());
    else if (a == "--url")
      url_opt = next();
    else if (a == "--base")
      base_dn = next();
    else if (a == "--filter")
      filter = next();
    else if (a == "--scope")
      scope = next();
    else if (a == "--attrs")
      attrs = next();
    else if (a == "--dn")
      dn_opt = next();
    else if (a == "--password")
      pw_opt = next();
    else if (a == "--bind-dn")
      bind_dn = next();
    else if (a == "--bind-pw")
      bind_pw = next();
    else if (!a.empty() && a[0] != '-')
      pos.push_back(a);
    else
      throw dslvm::Error("unknown ldap option: " + a);
  }

  lmcurl::Session sess(&rt, "");
  lmcurl::Request req(&sess, "GET", "");
  req.allow_error = true;
  if (!bind_dn.empty()) {
    req.auth_user = bind_dn;
    req.auth_password = bind_pw;
    if (!bind_pw.empty())
      rt.secret(bind_pw);
  }

  std::string base_url = !url_opt.empty() ? url_opt : ("ldap://" + host + ":" + std::to_string(port));
  if (sub == "ping") {
    req.verb = "GET";
    req.url = base_url + "/ping";
  } else if (sub == "bind") {
    std::string dn = !dn_opt.empty() ? dn_opt : (!pos.empty() ? pos[0] : bind_dn);
    std::string pw = !pw_opt.empty() ? pw_opt : (pos.size() > 1 ? pos[1] : bind_pw);
    req.verb = "POST";
    req.url = base_url + "/bind";
    req.body = lmcurl::Json({{"dn", dn}, {"password", pw}}).dump();
  } else if (sub == "whoami") {
    req.verb = "GET";
    req.url = base_url + "/whoami";
  } else if (sub == "search") {
    req.verb = "GET";
    if (!pos.empty() && is_url_arg(pos[0])) {
      req.url = pos[0];
    } else {
      req.url = base_url + "/" + pct_encode(base_dn) + "?" + attrs + "?" + scope + "?" +
                pct_encode(filter);
    }
  } else if (sub == "get") {
    std::string dn = !dn_opt.empty() ? dn_opt : (!pos.empty() ? pos[0] : "");
    req.verb = "GET";
    req.url = is_url_arg(dn) ? dn : (base_url + "/entry/" + pct_encode(dn));
  } else if (sub == "add") {
    if (pos.size() < 2)
      throw dslvm::Error("usage: lmcurl ldap add <DN> <JSON_ATTRS>");
    req.verb = "PUT";
    req.url = base_url + "/entry/" + pct_encode(pos[0]);
    req.body = pos[1];
  } else if (sub == "modify") {
    if (pos.size() < 2)
      throw dslvm::Error("usage: lmcurl ldap modify <DN> <JSON_MODS>");
    req.verb = "POST";
    req.url = base_url + "/entry/" + pct_encode(pos[0]);
    req.body = pos[1];
  } else if (sub == "compare") {
    if (pos.size() < 3)
      throw dslvm::Error("usage: lmcurl ldap compare <DN> <ATTR> <VALUE>");
    req.verb = "GET";
    req.url = base_url + "/compare?dn=" + pct_encode(pos[0]) +
              "&attr=" + pct_encode(pos[1]) + "&value=" + pct_encode(pos[2]);
  } else if (sub == "del" || sub == "delete") {
    if (pos.empty())
      throw dslvm::Error("usage: lmcurl ldap del <DN>");
    req.verb = "DELETE";
    req.url = base_url + "/entry/" + pct_encode(pos[0]);
  } else {
    throw dslvm::Error("unknown ldap subcommand: " + sub);
  }

  auto resp = req.send();
  std::cout << rt.redact(resp.body) << '\n';
  return (resp.status >= 200 && resp.status < 300) ? 0 : 1;
}

int run_cli_url_mode(lmcurl::Runtime &rt, int argc, char **argv) {
  lmcurl::Session sess(&rt, "");
  lmcurl::Request req(&sess, "GET", "");
  req.allow_error = true;
  bool verb_explicit = false;
  bool include_headers = false;
  bool fail_on_http_error = false;
  std::string target_url;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc)
        throw dslvm::Error("missing value for " + a);
      return argv[++i];
    };
    if (a == "-X" || a == "--request") {
      req.verb = next();
      verb_explicit = true;
    } else if (a == "-u" || a == "--user") {
      std::string cred = next();
      auto colon = cred.find(':');
      if (colon != std::string::npos) {
        req.auth_user = cred.substr(0, colon);
        req.auth_password = cred.substr(colon + 1);
        if (!req.auth_password.empty())
          rt.secret(req.auth_password);
      } else {
        req.auth_user = cred;
      }
      req.auth_kind = "basic";
    } else if (a == "-H" || a == "--header") {
      std::string h = next();
      auto colon = h.find(':');
      if (colon != std::string::npos) {
        req.headers[lmcurl::trim(h.substr(0, colon))] = lmcurl::trim(h.substr(colon + 1));
      }
    } else if (a == "-d" || a == "--data" || a == "--data-raw" || a == "--data-binary") {
      req.body = next();
      if (!verb_explicit)
        req.verb = "POST";
    } else if (a == "-i" || a == "--include") {
      include_headers = true;
    } else if (a == "-s" || a == "--silent" || a == "-S" || a == "--show-error") {
      // silent / show-error accepted for curl CLI compatibility
    } else if (a == "-L" || a == "--location") {
      req.follow = true;
    } else if (a == "-k" || a == "--insecure") {
      req.insecure = true;
    } else if (a == "-f" || a == "--fail") {
      fail_on_http_error = true;
    } else if (a == "--max-time" || a == "-m") {
      req.timeout_ms = static_cast<int64_t>(std::stod(next()) * 1000.0);
    } else if (is_url_arg(a)) {
      target_url = a;
    } else {
      return -1;
    }
  }
  if (target_url.empty())
    return -1;
  req.url = target_url;
  auto resp = req.send();
  if (!resp.error.empty()) {
    std::cerr << "lmcurl error: " << rt.redact(resp.error) << '\n';
    return 1;
  }
  if (include_headers) {
    std::cout << "HTTP/1.1 " << resp.status << "\r\n";
    for (const auto &[k, v] : resp.headers)
      std::cout << k << ": " << rt.redact(v) << "\r\n";
    std::cout << "\r\n";
  }
  std::cout << rt.redact(resp.body);
  if (!resp.body.empty() && resp.body.back() != '\n')
    std::cout << '\n';
  if (fail_on_http_error && resp.status >= 400)
    return 22;
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  std::unique_ptr<lmcurl::Runtime> runtime;
  try {
    runtime = std::make_unique<lmcurl::Runtime>();
    auto &rt = *runtime;
    dslvm::Registry registry;
    lmcurl::register_safe_std(registry, rt);
    lmcurl::register_http(registry, rt);
    lmcurl::register_framework(registry, rt);
    lmcurl::register_llm(registry, rt);
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "lmcurl " LMCURL_VERSION "\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--notices") {
      std::cout.write(reinterpret_cast<const char *>(lmcurl::embedded_notices),
                      lmcurl::embedded_notices_size);
      return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--lmcode-schema") {
      if (argc == 3 && std::string(argv[2]) == "--json") {
        std::cout.write(reinterpret_cast<const char *>(lmcurl::embedded_schema),
                        lmcurl::embedded_schema_size);
        return 0;
      }
      if (argc != 2)
        return usage_error("expected --lmcode-schema [--json]");
      for (const auto &s : registry.signatures())
        std::cout << s << '\n';
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--lmcode-signatures") {
      for (const auto &s : registry.signatures())
        std::cout << s << '\n';
      return 0;
    }
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
      std::cout
          << "lmcurl " LMCURL_VERSION
             " - libcurl REST, LDAPv3 and LLM client & lmcode host\n"
             "  lmcurl --lmcode FILE [FILE ...] [--dry-run] | --lmcode-schema [--json] | --lmcode-signatures | --version | --notices\n"
             "  lmcurl [-X METHOD] [-u bind_dn:pw] [-H 'Header: Val'] [-d BODY] [-i] <ldap://... | http://... | https://...>\n"
             "  lmcurl ldap <ping|bind|whoami|search|get|add|modify|compare|del> [options]\n"
             "LMCURL_MAX_MEMORY_MB: positive whole MiB (default 4096).\n"
             "LMCURL_LIVE=1 enables paid LLM calls; LMCURL_MAX_COST_USD defaults 0.02, hard run ceiling 0.25.\n"
             "Linux requires glibc >=2.35; all other libraries are static.\n";
      return 0;
    }
    // Check for direct CLI mode (`lmcurl ldap ...` or `lmcurl [flags] <ldap://...|http://...|https://...>`)
    // when `--lmcode` and retired `--dsl*` options are not used.
    bool has_lmcode_or_retired = false;
    bool has_url = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--lmcode" || a == "--dry-run" || a.rfind("--dsl", 0) == 0 || a.rfind("--lmcode", 0) == 0)
        has_lmcode_or_retired = true;
      if (is_url_arg(a))
        has_url = true;
    }
    if (!has_lmcode_or_retired && argc >= 2) {
      if (std::string(argv[1]) == "ldap") {
        return run_cli_ldap_subcommand(rt, argc, argv);
      }
      if (has_url) {
        int rc = run_cli_url_mode(rt, argc, argv);
        if (rc >= 0)
          return rc;
      }
    }
    // --lmcode: one invocation envelope (lmcode-cpp InvocationResults). Exit 0 success, 2 cli,
    // 3 load/run, 4 application (a failed test report) or output failure.
    bool dry_flag = false;
    std::vector<std::string> files;
    bool lmcode = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--lmcode" && !lmcode) {
        lmcode = true;
      } else if (a == "--dry-run" && !dry_flag) {
        dry_flag = true;
      } else if (lmcode && !a.empty() && a[0] != '-')
        files.push_back(a);
      else
        return usage_error("invalid argument");
    }
    if (!lmcode || files.empty())
      return usage_error("expected --lmcode FILE [FILE ...] [--dry-run]");
    // Dry mode is invocation-wide: run_lmcode supplies it in LMCODE_INVOCATION_FILE metadata; the
    // standalone --dry-run flag requests the same mode when no metadata is present.
    std::optional<dslvm::InvocationResults> holder;
    try {
      if (dry_flag && !std::getenv("LMCODE_INVOCATION_FILE"))
        holder.emplace(dslvm::InvocationResults::from_command(files, true));
      else
        holder.emplace(dslvm::InvocationResults::from_command(files));
    } catch (const dslvm::Error &e) {
      return dslvm::failure_frame("cli", e);
    }
    auto &results = *holder;
    if (dry_flag && !results.dry())
      return usage_error("--dry-run contradicts real-mode invocation metadata");
    if (const char *value = std::getenv("LMCURL_MAX_MEMORY_MB")) {
      std::string v = value;
      bool valid = !v.empty() && v.find_first_not_of("0123456789") == std::string::npos;
      try {
        if (!valid || std::stoull(v) == 0 || std::stoull(v) > SIZE_MAX / (1024 * 1024))
          valid = false;
      } catch (...) {
        valid = false;
      }
      if (!valid) {
        std::cerr << "LMCURL_MAX_MEMORY_MB must be positive whole MiB\n";
        return usage_error("invalid LMCURL_MAX_MEMORY_MB");
      }
    }
    if (!membudget::configure("lmcurl", "LMCURL_MAX_MEMORY_MB"))
      return usage_error("invalid LMCURL_MAX_MEMORY_MB");
    if (const char *v = std::getenv("LMCURL_MAX_COST_USD")) {
      std::string s = v;
      std::size_t n = 0;
      try {
        rt.llm_cap = std::stod(s, &n);
      } catch (...) {
        return usage_error("invalid LMCURL_MAX_COST_USD");
      }
      if (n != s.size() || !std::isfinite(rt.llm_cap) || rt.llm_cap < 0 || rt.llm_cap > 0.25)
        return usage_error("LMCURL_MAX_COST_USD must be in 0..0.25");
    }
    std::string schema(reinterpret_cast<const char *>(lmcurl::embedded_schema),
                       lmcurl::embedded_schema_size);
    // A program that exhausted the memory budget still needs a failure frame: this reserve is
    // released first, so recording the failure cannot throw out of a handler.
    auto reserve = std::make_unique<char[]>(1 << 20);
    auto fail = [&](std::optional<std::size_t> part, const char *phase, const dslvm::Error &e) {
      reserve.reset();
      auto safe = e;
      safe.message = rt.redact(e.message);
      std::cerr << safe.message << '\n';
      results.fail(part, phase, safe);
    };
    dslvm::ResultContract contract;
    try {
      contract = dslvm::ResultContract::from_schema(schema);
    } catch (const dslvm::Error &e) {
      fail(std::nullopt, "load", e);
      return emit_frame(rt, results);
    }
    // Load and verify every file first: a bad later file must not leave earlier effects behind.
    std::vector<std::pair<std::size_t, dslvm::Program>> programs;
    for (const auto &part : results.registrations())
      try {
        programs.emplace_back(part.index,
                              dslvm::Program::load(lmcurl::read_file(part.file), schema, registry));
      } catch (const dslvm::Error &e) {
        fail(part.index, "load", e);
        return emit_frame(rt, results);
      } catch (const std::exception &e) {
        fail(part.index, "load", dslvm::Error(e.what()));
        return emit_frame(rt, results);
      }
    dslvm::Context context;
    context.set("curl", &rt);
    context.set("llm", &rt);
    for (auto &[index, program] : programs)
      try {
        results.select(index, registry, contract);
        // A dry invocation loads every program and runs none: there is no effect-free request.
        if (!results.dry()) {
          program.run(context, registry);
          if (rt.failed)
            throw dslvm::Error("TEST_FAILED", "a test report failed assert_all");
        }
        results.complete(index);
      } catch (const dslvm::Error &e) {
        fail(index, e.code == "APP_FAIL" || e.code == "TEST_FAILED" ? "app" : "run", e);
        break;
      } catch (const std::exception &e) { // includes the memory budget
        fail(index, "run", dslvm::Error(e.what()));
        break;
      }
    return emit_frame(rt, results);
  } catch (const std::exception &e) {
    auto text = runtime ? runtime->redact(e.what()) : std::string("initialization failed");
    std::cerr << "lmcurl error: " << text << '\n';
    return dslvm::failure_frame("run", dslvm::Error(text));
  }
}
