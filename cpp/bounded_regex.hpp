#pragma once
#include "core.hpp"
#include <regex.h>
#include <array>
namespace lmcurl {
// POSIX ERE without backreferences avoids the recursive backtracking used by std::regex.
// The Linux-only full host uses libc's automaton; other platforms ship metadata stubs.
inline std::vector<std::string> regex_groups(const std::string &text, const std::string &pattern) {
  if (text.size() > 8192 || pattern.size() > 256 || text.find('\0') != std::string::npos ||
      pattern.find('\0') != std::string::npos)
    throw dslvm::Error("regex limits exceeded (8192 text bytes / 256 pattern bytes)");
  std::string translated;
  bool bracket = false;
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    char c = pattern[i];
    if (c == '\\' && i + 1 < pattern.size()) {
      char next = pattern[++i];
      if (next >= '1' && next <= '9')
        throw dslvm::Error("regex backreferences are unsupported");
      std::string cls;
      if (next == 'd')
        cls = "[:digit:]";
      if (next == 'w')
        cls = "[:alnum:]_";
      if (next == 's')
        cls = "[:space:]";
      if (!cls.empty())
        translated += bracket ? cls : "[" + cls + "]";
      else {
        translated += '\\';
        translated += next;
      }
    } else {
      if (c == '[')
        bracket = true;
      if (c == ']')
        bracket = false;
      translated += c;
    }
  }
  Reservation workspace(8 * 1024 * 1024);
  regex_t regex{};
  if (regcomp(&regex, translated.c_str(), REG_EXTENDED) != 0)
    throw dslvm::Error("invalid POSIX extended regex");
  struct Cleanup {
    regex_t *p;
    ~Cleanup() { regfree(p); }
  } cleanup{&regex};
  std::array<regmatch_t, 33> matches{};
  int status = regexec(&regex, text.c_str(), matches.size(), matches.data(), 0);
  if (status == REG_NOMATCH)
    return {};
  if (status != 0)
    throw dslvm::Error("regex execution failed");
  std::vector<std::string> result;
  for (std::size_t i = 0; i <= regex.re_nsub && i < matches.size(); ++i)
    result.push_back(matches[i].rm_so < 0
                         ? ""
                         : text.substr(matches[i].rm_so, matches[i].rm_eo - matches[i].rm_so));
  return result;
}
inline bool regex_matches(const std::string &text, const std::string &pattern) {
  return !regex_groups(text, pattern).empty();
}
} // namespace lmcurl
