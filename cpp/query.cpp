#include "query.hpp"
#include <valijson/adapters/nlohmann_json_adapter.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validation_results.hpp>
#include <valijson/validator.hpp>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include "bounded_regex.hpp"
#include <sstream>
namespace lmcurl {
namespace {
constexpr std::size_t max_input = 8 * 1024 * 1024;
Json missing_json() { return Json(); }
bool safe_path(const std::string &p) {
  return p.size() <= 2048 && std::count(p.begin(), p.end(), '[') <= 128;
}
} // namespace
Json select_json(const Json &root, const std::string &path, bool &found) {
  struct Token {
    std::string key;
    int64_t index = -1;
    bool wildcard = false;
  };
  std::vector<Token> tokens;
  if (path.empty() || path.size() > 2048)
    throw dslvm::Error("invalid JSONPath length");
  std::size_t i = path[0] == '$' ? 1 : 0;
  while (i < path.size()) {
    if (path[i] == '.') {
      ++i;
      if (i == path.size() || path[i] == '.')
        throw dslvm::Error("unsupported JSONPath syntax");
    }
    Token t;
    if (path[i] == '[') {
      ++i;
      if (i >= path.size())
        throw dslvm::Error("unclosed JSONPath index");
      if (path[i] == '*') {
        t.wildcard = true;
        ++i;
      } else if (path[i] == '\"' || path[i] == '\'') {
        char quote = path[i++];
        bool closed = false;
        while (i < path.size()) {
          char c = path[i++];
          if (c == quote) {
            closed = true;
            break;
          }
          if (c == '\\' && i < path.size())
            c = path[i++];
          t.key += c;
        }
        if (!closed)
          throw dslvm::Error("unclosed JSONPath property");
      } else {
        auto start = i;
        while (i < path.size() && std::isdigit(static_cast<unsigned char>(path[i])))
          ++i;
        if (i == start)
          throw dslvm::Error("unsupported JSONPath index");
        t.index = std::stoll(path.substr(start, i - start));
      }
      if (i >= path.size() || path[i++] != ']')
        throw dslvm::Error("unclosed JSONPath index");
    } else {
      auto start = i;
      while (i < path.size() && path[i] != '.' && path[i] != '[')
        ++i;
      t.key = path.substr(start, i - start);
      if (t.key == "*")
        t.wildcard = true;
      if (t.key.empty() && !t.wildcard)
        throw dslvm::Error("empty JSONPath property");
    }
    tokens.push_back(t);
    if (tokens.size() > 64)
      throw dslvm::Error("JSONPath exceeds 64 selectors");
  }
  std::function<Json(const Json &, std::size_t, bool &)> walk = [&](const Json &cur, std::size_t at,
                                                                    bool &ok) -> Json {
    if (at == tokens.size()) {
      ok = true;
      return cur;
    }
    const auto &t = tokens[at];
    if (t.wildcard) {
      Json result = Json::array();
      if (!cur.is_array() && !cur.is_object()) {
        ok = false;
        return nullptr;
      }
      for (const auto &item : cur) {
        bool match = false;
        auto value = walk(item, at + 1, match);
        if (match)
          result.push_back(std::move(value));
        if (result.size() > 10000)
          throw dslvm::Error("JSONPath result limit exceeded");
      }
      ok = !result.empty();
      return result;
    }
    if (t.index >= 0) {
      if (!cur.is_array() || static_cast<uint64_t>(t.index) >= cur.size()) {
        ok = false;
        return nullptr;
      }
      return walk(cur[static_cast<std::size_t>(t.index)], at + 1, ok);
    }
    if (!cur.is_object() || !cur.contains(t.key)) {
      ok = false;
      return nullptr;
    }
    return walk(cur[t.key], at + 1, ok);
  };
  found = false;
  return walk(root, 0, found);
}
bool schema_valid(const Json &value, const std::string &schema_text) {
  try {
    if (schema_text.size() > max_input)
      return false;
    Json sj = parse_json(schema_text);
    valijson::Schema schema;
    valijson::SchemaParser parser;
    valijson::adapters::NlohmannJsonAdapter sa(sj), ta(value);
    parser.populateSchema(sa, schema);
    valijson::Validator validator;
    valijson::ValidationResults errors;
    return validator.validate(schema, ta, &errors);
  } catch (...) {
    return false;
  }
}
std::string xpath_value(const std::string &body, const std::string &expr, bool html, bool &found) {
  found = false;
  if (body.size() > max_input || expr.size() > 4096)
    return {};
  xmlDocPtr doc =
      html ? htmlReadMemory(body.data(), static_cast<int>(body.size()), "response", nullptr,
                            HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING)
           : xmlReadMemory(body.data(), static_cast<int>(body.size()), "response", nullptr,
                           XML_PARSE_NO_XXE | XML_PARSE_NONET | XML_PARSE_NOERROR |
                               XML_PARSE_NOWARNING);
  if (!doc)
    return {};
  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  if (!ctx) {
    xmlFreeDoc(doc);
    return {};
  }
  ctx->error = +[](void *, const xmlError *) {};
  xmlXPathObjectPtr obj =
      xmlXPathEvalExpression(reinterpret_cast<const xmlChar *>(expr.c_str()), ctx);
  std::string out;
  if (obj) {
    if (obj->type == XPATH_STRING && obj->stringval)
      out = reinterpret_cast<const char *>(obj->stringval);
    else if (obj->type == XPATH_BOOLEAN)
      out = obj->boolval ? "true" : "false";
    else if (obj->type == XPATH_NUMBER)
      out = std::to_string(obj->floatval);
    else if (obj->type == XPATH_NODESET && obj->nodesetval && obj->nodesetval->nodeNr) {
      xmlChar *txt = xmlNodeGetContent(obj->nodesetval->nodeTab[0]);
      if (txt) {
        out = reinterpret_cast<const char *>(txt);
        xmlFree(txt);
      }
    }
    xmlXPathFreeObject(obj);
  }
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
  found = !out.empty();
  return out;
}
static bool numeric(const std::string &s, double &v) {
  try {
    std::size_t n = 0;
    v = std::stod(s, &n);
    return n == s.size() && std::isfinite(v);
  } catch (...) {
    return false;
  }
}
static TypedQuery *tq(Runtime &r, Json v, bool m = false) {
  return r.hold<TypedQuery>(&r, std::move(v), m);
}
static TypedQuery *tqs(Runtime &r, const std::string &s, bool m = false) {
  return r.hold<TypedQuery>(&r, s, m);
}
void register_queries(dslvm::Registry &r, Runtime &rt) {
  method(r, rt, "Curl", "query_json", {"string", "string"}, "Query", [&rt](void *, Args &a) {
    try {
      Json j = parse_json(a[0].s);
      bool found = false;
      auto v = select_json(j, a[1].s, found);
      return Value::object("Query", tq(rt, v, !found));
    } catch (...) {
      return Value::object("Query", tq(rt, Json(), true));
    }
  });
  method(r, rt, "Curl", "query_text", {"string"}, "Query",
         [&rt](void *, Args &a) { return Value::object("Query", tqs(rt, a[0].s)); });
  method(r, rt, "Curl", "schema_valid", {"string", "string"}, "bool", [](void *, Args &a) {
    try {
      return Value::boolean(schema_valid(parse_json(a[0].s), a[1].s));
    } catch (...) {
      return Value::boolean(false);
    }
  });
  method(r, rt, "Response", "query", {"string", "string"}, "Query", [&rt](void *p, Args &a) {
    auto &x = *static_cast<Response *>(p);
    auto kind = lower(a[0].s);
    if (kind == "json") {
      bool f = false;
      auto v = select_json(parse_json(x.body), a[1].s, f);
      return Value::object("Query", tq(rt, std::move(v), !f));
    }
    if (kind == "xpath") {
      bool f = false;
      auto value = xpath_value(x.body, a[1].s,
                               x.headers.count("content-type") &&
                                   x.headers.at("content-type").find("html") != std::string::npos,
                               f);
      return Value::object("Query", tqs(rt, value, !f));
    }
    if (kind == "body")
      return Value::object("Query", tqs(rt, x.body));
    if (kind == "status")
      return Value::object("Query", tq(rt, x.status));
    if (kind == "bytes")
      return Value::object("Query", tq(rt, x.size));
    if (kind == "sha256")
      return Value::object("Query", tqs(rt, sha256(x.body)));
    if (kind == "duration")
      return Value::object("Query", tq(rt, a[1].s == "ttfb"      ? x.ttfb_ms
                                           : a[1].s == "dns"     ? x.dns_ms
                                           : a[1].s == "connect" ? x.connect_ms
                                           : a[1].s == "tls"     ? x.tls_ms
                                                                 : x.total_ms));
    if (kind == "header") {
      auto it = x.headers.find(lower(a[1].s));
      return Value::object("Query",
                           tqs(rt, it == x.headers.end() ? "" : it->second, it == x.headers.end()));
    }
    if (kind == "certificate") {
      std::string value = x.certificate;
      if (!a[1].s.empty()) {
        value.clear();
        std::istringstream in(x.certificate);
        std::string line;
        while (std::getline(in, line)) {
          auto colon = line.find(':');
          if (colon != std::string::npos && lower(line.substr(0, colon)) == lower(a[1].s)) {
            value = trim(line.substr(colon + 1));
            break;
          }
        }
      }
      return Value::object("Query", tqs(rt, value, value.empty()));
    }
    if (kind == "cookie") {
      auto it = x.headers.find("set-cookie");
      if (it != x.headers.end()) {
        std::istringstream in(it->second);
        std::string line;
        while (std::getline(in, line)) {
          auto eq = line.find('=');
          if (line.substr(0, eq) == a[1].s) {
            auto semi = line.find(';', eq);
            return Value::object("Query", tqs(rt, line.substr(eq + 1, semi - eq - 1)));
          }
        }
      }
      return Value::object("Query", tqs(rt, "", true));
    }
    if (kind == "regex") {
      if (a[1].s.size() > 256 || x.body.size() > 8192)
        throw dslvm::Error("regex query limits exceeded");
      auto m = regex_groups(x.body, a[1].s);
      if (!m.empty())
        return Value::object("Query", tqs(rt, m.size() > 1 ? m[1] : m[0]));
      return Value::object("Query", tqs(rt, "", true));
    }
    throw dslvm::Error("unsupported response query kind");
  });
  method(r, rt, "Response", "json", {"string"}, "Query", [&rt](void *p, Args &a) {
    bool f = false;
    Json j = parse_json(static_cast<Response *>(p)->body);
    auto value = select_json(j, a[0].s, f);
    return Value::object("Query", tq(rt, std::move(value), !f));
  });
  method(r, rt, "Response", "xpath", {"string"}, "Query", [&rt](void *p, Args &a) {
    bool f = false;
    auto value = xpath_value(
        static_cast<Response *>(p)->body, a[0].s,
        static_cast<Response *>(p)->headers["content-type"].find("html") != std::string::npos, f);
    return Value::object("Query", tqs(rt, value, !f));
  });
  method(r, rt, "Response", "assert_openapi", {"string", "string", "string"}, "bool",
         [](void *p, Args &a) {
           try {
             auto spec = parse_json(a[0].s);
             auto &response = *static_cast<Response *>(p);
             auto operation = spec.at("paths").at(a[1].s).at(lower(a[2].s));
             auto responses = operation.at("responses");
             auto key = std::to_string(response.status);
             if (!responses.contains(key))
               key = "default";
             auto contract = responses.at(key);
             if (!contract.contains("content"))
               return Value::boolean(response.body.empty());
             auto schema = contract.at("content").at("application/json").at("schema");
             Json root = spec;
             root["allOf"] = Json::array({schema});
             return Value::boolean(schema_valid(parse_json(response.body), root.dump()));
           } catch (...) {
             return Value::boolean(false);
           }
         });
  method(r, rt, "Response", "assert_json_schema", {"string"}, "bool", [](void *p, Args &a) {
    try {
      Json i = parse_json(static_cast<Response *>(p)->body), s = parse_json(a[0].s);
      valijson::Schema schema;
      valijson::SchemaParser parser;
      valijson::adapters::NlohmannJsonAdapter sa(s), ta(i);
      parser.populateSchema(sa, schema);
      valijson::Validator v;
      valijson::ValidationResults e;
      return Value::boolean(v.validate(schema, ta, &e));
    } catch (...) {
      return Value::boolean(false);
    }
  });
  method(r, rt, "Query", "value", {}, "string",
         [](void *p, Args &) { return Value::string(static_cast<TypedQuery *>(p)->text); });
  method(r, rt, "Query", "missing", {}, "bool",
         [](void *p, Args &) { return Value::boolean(static_cast<TypedQuery *>(p)->missing); });
  method(r, rt, "Query", "exists", {}, "bool",
         [](void *p, Args &) { return Value::boolean(!static_cast<TypedQuery *>(p)->missing); });
  method(r, rt, "Query", "is_string", {}, "bool", [](void *p, Args &) {
    auto &q = *static_cast<TypedQuery *>(p);
    return Value::boolean(!q.missing && q.value.is_string());
  });
  method(r, rt, "Query", "is_collection", {}, "bool", [](void *p, Args &) {
    auto &q = *static_cast<TypedQuery *>(p);
    return Value::boolean(!q.missing && (q.value.is_array() || q.value.is_object()));
  });
  method(r, rt, "Query", "is_empty", {}, "bool", [](void *p, Args &) {
    auto &q = *static_cast<TypedQuery *>(p);
    return Value::boolean(q.missing || (q.value.is_string() && q.text.empty()) ||
                          (q.value.is_array() && q.value.empty()) ||
                          (q.value.is_object() && q.value.empty()));
  });
  method(r, rt, "Query", "count", {}, "int", [](void *p, Args &) {
    auto &q = *static_cast<TypedQuery *>(p);
    if (q.missing)
      return Value::integer(0);
    if (q.value.is_array() || q.value.is_object())
      return Value::integer(q.value.size());
    return Value::integer(1);
  });
  method(r, rt, "Query", "nth", {"int"}, "Query", [&rt](void *p, Args &a) {
    auto &q = *static_cast<TypedQuery *>(p);
    if (!q.value.is_array() || a[0].i < 0 || static_cast<std::size_t>(a[0].i) >= q.value.size())
      return Value::object("Query", tq(rt, Json(), true));
    return Value::object("Query", tq(rt, q.value[static_cast<std::size_t>(a[0].i)]));
  });
  for (const char *op :
       {"equals", "not_equals", "contains", "starts_with", "ends_with", "matches", "includes"})
    method(r, rt, "Query", op, {"string"}, "bool", [op](void *p, Args &a) {
      auto &q = *static_cast<TypedQuery *>(p);
      if (q.missing)
        return Value::boolean(false);
      if (op == std::string("equals"))
        return Value::boolean(q.text == a[0].s);
      if (op == std::string("not_equals"))
        return Value::boolean(q.text != a[0].s);
      if (op == std::string("includes") && q.value.is_array()) {
        for (const auto &v : q.value)
          if ((v.is_string() ? v.get<std::string>() : v.dump()) == a[0].s)
            return Value::boolean(true);
        return Value::boolean(false);
      }
      if (op == std::string("contains") || op == std::string("includes"))
        return Value::boolean(q.text.find(a[0].s) != std::string::npos);
      if (op == std::string("starts_with"))
        return Value::boolean(q.text.rfind(a[0].s, 0) == 0);
      if (op == std::string("ends_with"))
        return Value::boolean(
            q.text.size() >= a[0].s.size() &&
            q.text.compare(q.text.size() - a[0].s.size(), a[0].s.size(), a[0].s) == 0);
      if (q.text.size() > 8192 || a[0].s.size() > 256)
        throw dslvm::Error("regex limits exceeded");
      try {
        return Value::boolean(regex_matches(q.text, a[0].s));
      } catch (...) {
        return Value::boolean(false);
      }
    });
  for (const char *op : {"gt", "lt"})
    method(r, rt, "Query", op, {"string"}, "bool", [op](void *p, Args &a) {
      double x = 0, y = 0;
      return Value::boolean(numeric(static_cast<TypedQuery *>(p)->text, x) && numeric(a[0].s, y) &&
                            (op == std::string("gt") ? x > y : x < y));
    });
  method(r, rt, "Query", "replace", {"string", "string"}, "Query", [&rt](void *p, Args &a) {
    auto s = static_cast<TypedQuery *>(p)->text;
    if (a[0].s.empty())
      throw dslvm::Error("replacement pattern cannot be empty");
    size_t i = 0;
    while ((i = s.find(a[0].s, i)) != std::string::npos) {
      s.replace(i, a[0].s.size(), a[1].s);
      i += a[1].s.size();
    }
    return Value::object("Query", tqs(rt, s));
  });
  method(r, rt, "Query", "split", {"string"}, "Query", [&rt](void *p, Args &a) {
    Json j = Json::array();
    auto &s = static_cast<TypedQuery *>(p)->text;
    if (a[0].s.empty())
      throw dslvm::Error("split separator cannot be empty");
    size_t b = 0, e;
    while ((e = s.find(a[0].s, b)) != std::string::npos) {
      j.push_back(s.substr(b, e - b));
      b = e + a[0].s.size();
    }
    j.push_back(s.substr(b));
    return Value::object("Query", tq(rt, j));
  });
  method(r, rt, "Query", "regex", {"string"}, "Query", [&rt](void *p, Args &a) {
    auto m = regex_groups(static_cast<TypedQuery *>(p)->text, a[0].s);
    if (m.empty())
      return Value::object("Query", tqs(rt, "", true));
    return Value::object("Query", tqs(rt, m[0]));
  });
  method(r, rt, "Query", "is_integer", {}, "bool", [](void *p, Args &) {
    auto &q = *static_cast<TypedQuery *>(p);
    try {
      std::size_t n = 0;
      std::stoll(q.text, &n);
      return Value::boolean(!q.missing && n == q.text.size());
    } catch (...) {
      return Value::boolean(false);
    }
  });
  method(r, rt, "Query", "to_int", {}, "int", [](void *p, Args &) {
    auto &s = static_cast<TypedQuery *>(p)->text;
    try {
      std::size_t n = 0;
      auto v = std::stoll(s, &n);
      if (n != s.size())
        throw std::invalid_argument("trailing");
      return Value::integer(v);
    } catch (...) {
      throw dslvm::Error("query value is not integer");
    }
  });
  method(r, rt, "Query", "html_escape", {}, "Query", [&rt](void *p, Args &) {
    return Value::object("Query", tqs(rt, html_escape(static_cast<TypedQuery *>(p)->text)));
  });
  method(r, rt, "Query", "url_decode", {}, "Query", [&rt](void *p, Args &) {
    auto &s = static_cast<TypedQuery *>(p)->text;
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '%' && i + 2 < s.size()) {
        auto h = [](char c) {
          if (c >= '0' && c <= '9')
            return c - '0';
          if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
          if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
          return -1;
        };
        int a = h(s[i + 1]), b = h(s[i + 2]);
        if (a >= 0 && b >= 0) {
          out.push_back(char(a * 16 + b));
          i += 2;
          continue;
        }
      }
      out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return Value::object("Query", tqs(rt, out));
  });
  method(r, rt, "Query", "to_date", {"string"}, "Query", [&rt](void *p, Args &a) {
    std::tm tm{};
    std::istringstream in(static_cast<TypedQuery *>(p)->text);
    in >> std::get_time(&tm, a[0].s.c_str());
    if (in.fail())
      return Value::object("Query", tqs(rt, "", true));
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return Value::object("Query", tqs(rt, out.str()));
  });
  method(r, rt, "Query", "format", {"string"}, "Query", [&rt](void *p, Args &a) {
    auto templ = a[0].s;
    auto pos = templ.find("{}");
    if (pos != std::string::npos)
      templ.replace(pos, 2, static_cast<TypedQuery *>(p)->text);
    return Value::object("Query", tqs(rt, templ));
  });
}
} // namespace lmcurl
