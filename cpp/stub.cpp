#include <iostream>
#include <string>
#include "schema_embedded.hpp"
#include "notices_embedded.hpp"
#ifndef LMCURL_VERSION
#define LMCURL_VERSION "0.1.0"
#endif
int main(int argc,char**argv){
    std::string a=argc>1?argv[1]:"";
    if(a=="--version"){std::cout<<"lmcurl " LMCURL_VERSION "\n";return 0;}
    if(a=="--notices"){std::cout.write(reinterpret_cast<const char*>(lmcurl::embedded_notices),lmcurl::embedded_notices_size);return 0;}
    if(a=="--dsl-schema"&&argc==3&&std::string(argv[2])=="--json"){std::cout.write(reinterpret_cast<const char*>(lmcurl::embedded_schema),lmcurl::embedded_schema_size);return 0;}
    if(a=="--help"){std::cout<<"lmcurl: this platform is a stub; network/DSL execution is not supported yet. Linux has the complete implementation.\n";return 0;}
    std::cerr<<"lmcurl: network/DSL execution is not supported on this platform yet\n";return a=="--dsl"?3:2;
}
