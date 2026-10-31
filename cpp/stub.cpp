#include <iostream>
#include <string>
#include "schema_embedded.hpp"
#include "signatures_embedded.hpp"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "notices_embedded.hpp"
#ifndef LMCURL_VERSION
#define LMCURL_VERSION "0.1.1"
#endif
int main(int argc, char **argv) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  std::string a = argc > 1 ? argv[1] : "";
  if (a == "--version" && argc == 2) {
    std::cout << "lmcurl " LMCURL_VERSION "\n";
    return 0;
  }
  if (a == "--notices" && argc == 2) {
    std::cout.write(reinterpret_cast<const char *>(lmcurl::embedded_notices),
                    lmcurl::embedded_notices_size);
    return 0;
  }
  if (a == "--lmcode-schema" && argc == 3 && std::string(argv[2]) == "--json") {
    std::cout.write(reinterpret_cast<const char *>(lmcurl::embedded_schema),
                    lmcurl::embedded_schema_size);
    return 0;
  }
  if ((a == "--lmcode-signatures" || a == "--lmcode-schema") && argc == 2) {
    std::cout.write(reinterpret_cast<const char *>(lmcurl::embedded_signatures),
                    lmcurl::embedded_signatures_size);
    return 0;
  }
  if (a == "--help" && argc == 2) {
    std::cout << "lmcurl: this platform is a stub; network/DSL execution is not supported yet. "
                 "Linux has the complete implementation.\n";
    return 0;
  }
  std::cerr << "lmcurl: network/DSL execution is not supported on this platform yet\n";
  return a == "--lmcode" ? 3 : 2;
}
