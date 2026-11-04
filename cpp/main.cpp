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
#define LMCURL_VERSION "0.1.1"
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
             " - libcurl REST and LLM tests\n--lmcode FILE [FILE ...] [--dry-run] | --lmcode-schema "
             "[--json] | --lmcode-signatures | --version | --notices\nLMCURL_MAX_MEMORY_MB: positive "
             "whole MiB (default 4096).\nLMCURL_LIVE=1 enables paid LLM calls; LMCURL_MAX_COST_USD "
             "defaults 0.02, hard run ceiling 0.25.\nLinux requires glibc >=2.35; all other "
             "libraries are static.\n";
      return 0;
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
