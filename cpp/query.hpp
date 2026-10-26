#pragma once
#include "core.hpp"
namespace lmcurl {
Json select_json(const Json&, const std::string& path, bool& found);
bool schema_valid(const Json&, const std::string& schema);
std::string xpath_value(const std::string&, const std::string&, bool html, bool& found);
void register_queries(dslvm::Registry&, Runtime&);
struct TypedQuery {
  Runtime* runtime{}; Json value; std::string text; bool missing=false;
  TypedQuery()=default;
  TypedQuery(Runtime*r, Json v, bool m=false):runtime(r),value(std::move(v)),missing(m){text=value.is_string()?value.get<std::string>():value.dump();}
  TypedQuery(Runtime*r, std::string s, bool m=false):runtime(r),text(std::move(s)),missing(m){value=text;}
};
}
