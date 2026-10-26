#include "core.hpp"
#include "schema_embedded.hpp"
#include "notices_embedded.hpp"
#include <cstdlib>
#include <iostream>
#ifndef LMCURL_VERSION
#define LMCURL_VERSION "0.1.1"
#endif
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
    if (argc >= 2 && std::string(argv[1]) == "--dsl-schema") {
      if (argc == 3 && std::string(argv[2]) == "--json") {
        std::cout.write(reinterpret_cast<const char *>(lmcurl::embedded_schema),
                        lmcurl::embedded_schema_size);
        return 0;
      }
      if (argc != 2)
        return 2;
      for (const auto &s : registry.signatures())
        std::cout << s << '\n';
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--dsl-signatures") {
      for (const auto &s : registry.signatures())
        std::cout << s << '\n';
      return 0;
    }
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
      std::cout
          << "lmcurl " LMCURL_VERSION
             " - libcurl REST and LLM tests\n--dsl FILE [FILE ...] [--dry-run] | --dsl-schema "
             "[--json] | --dsl-signatures | --version | --notices\nLMCURL_MAX_MEMORY_MB: positive "
             "whole MiB (default 4096).\nLMCURL_LIVE=1 enables paid LLM calls; LMCURL_MAX_COST_USD "
             "defaults 0.02, hard run ceiling 0.25.\nLinux requires glibc >=2.35; all other "
             "libraries are static.\n";
      return 0;
    }
    bool dry = false;
    std::vector<std::string> files;
    bool dsl = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--dsl" && !dsl) {
        dsl = true;
      } else if (a == "--dry-run" && !dry) {
        dry = true;
      } else if (dsl && !a.empty() && a[0] != '-')
        files.push_back(a);
      else
        return 2;
    }
    if (!dsl || files.empty())
      return 2;
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
        return 2;
      }
    }
    if (!membudget::configure("lmcurl", "LMCURL_MAX_MEMORY_MB"))
      return 2;
    if (const char *v = std::getenv("LMCURL_MAX_COST_USD")) {
      std::string s = v;
      std::size_t n = 0;
      try {
        rt.llm_cap = std::stod(s, &n);
      } catch (...) {
        throw dslvm::Error("invalid LMCURL_MAX_COST_USD");
      }
      if (n != s.size() || !std::isfinite(rt.llm_cap) || rt.llm_cap < 0 || rt.llm_cap > 0.25)
        throw dslvm::Error("LMCURL_MAX_COST_USD must be in 0..0.25");
    }
    std::string schema(reinterpret_cast<const char *>(lmcurl::embedded_schema),
                       lmcurl::embedded_schema_size);
    std::vector<dslvm::Program> programs;
    for (const auto &file : files)
      try {
        programs.push_back(dslvm::Program::load(lmcurl::read_file(file), schema, registry));
      } catch (const std::exception &e) {
        std::cerr << rt.redact(e.what()) << '\n';
        return dslvm::report_error("load", dslvm::Error(rt.redact(e.what())));
      }
    if (!dry) {
      dslvm::Context context;
      context.set("curl", &rt);
      context.set("llm", &rt);
      for (const auto &program : programs)
        try {
          program.run(context, registry);
        } catch (const std::exception &e) {
          auto text = rt.redact(e.what());
          std::cerr << text << '\n';
          return dslvm::report_error("run", dslvm::Error(text));
        }
    }
    if (rt.failed) {
      std::cout << "{\"ok\":false,\"phase\":\"test\",\"code\":\"TEST_FAILED\"}\n";
      return 1;
    }
    dslvm::report_ok();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "lmcurl error: " << (runtime ? runtime->redact(e.what()) : "initialization failed")
              << '\n';
    return 3;
  }
}
