#pragma once
#include "core.hpp"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace lmcurl::ldap_wire {

inline std::string ber_len(std::size_t len) {
  if (len < 128)
    return std::string(1, static_cast<char>(len));
  std::string bytes;
  std::size_t n = len;
  while (n > 0) {
    bytes.insert(bytes.begin(), static_cast<char>(n & 0xff));
    n >>= 8;
  }
  std::string out(1, static_cast<char>(0x80 | bytes.size()));
  out += bytes;
  return out;
}

inline std::string ber_tag(uint8_t tag, const std::string &content) {
  std::string out(1, static_cast<char>(tag));
  out += ber_len(content.size());
  out += content;
  return out;
}

inline std::string ber_int(int64_t val, uint8_t tag = 0x02) {
  if (val == 0)
    return ber_tag(tag, std::string(1, '\0'));
  std::string bytes;
  uint64_t v = static_cast<uint64_t>(val);
  while (v > 0) {
    bytes.insert(bytes.begin(), static_cast<char>(v & 0xff));
    v >>= 8;
  }
  if (static_cast<uint8_t>(bytes[0]) & 0x80)
    bytes.insert(bytes.begin(), '\0');
  return ber_tag(tag, bytes);
}

inline std::string ber_enum(int64_t val) { return ber_int(val, 0x0a); }

inline std::string ber_bool(bool val) {
  return ber_tag(0x01, std::string(1, val ? static_cast<char>(0xff) : '\0'));
}

inline std::string ber_octet(const std::string &s, uint8_t tag = 0x04) {
  return ber_tag(tag, s);
}

inline std::string ber_seq(const std::vector<std::string> &items, uint8_t tag = 0x30) {
  std::string joined;
  for (const auto &it : items)
    joined += it;
  return ber_tag(tag, joined);
}

inline std::string ber_set(const std::vector<std::string> &items) {
  return ber_seq(items, 0x31);
}

struct BerNode {
  uint8_t tag = 0;
  std::string value;
  std::size_t next = 0;
  bool needed = false;
};

inline BerNode read_ber_node(const std::string &buf, std::size_t offset = 0) {
  BerNode n;
  if (offset + 2 > buf.size()) {
    n.needed = true;
    return n;
  }
  n.tag = static_cast<uint8_t>(buf[offset]);
  uint8_t lb = static_cast<uint8_t>(buf[offset + 1]);
  std::size_t len = 0;
  std::size_t hdr = 2;
  if ((lb & 0x80) == 0) {
    len = lb;
  } else {
    std::size_t nb = lb & 0x7f;
    if (nb == 0 || nb > 4 || offset + 2 + nb > buf.size()) {
      n.needed = true;
      return n;
    }
    for (std::size_t i = 0; i < nb; ++i)
      len = (len << 8) | static_cast<uint8_t>(buf[offset + 2 + i]);
    hdr = 2 + nb;
  }
  if (offset + hdr + len > buf.size()) {
    n.needed = true;
    return n;
  }
  n.value = buf.substr(offset + hdr, len);
  n.next = offset + hdr + len;
  return n;
}

inline std::vector<BerNode> read_ber_children(const std::string &buf) {
  std::vector<BerNode> out;
  std::size_t off = 0;
  while (off < buf.size()) {
    auto node = read_ber_node(buf, off);
    if (node.needed)
      break;
    out.push_back(std::move(node));
    off = out.back().next;
  }
  return out;
}

inline int64_t decode_ber_int(const std::string &v) {
  int64_t n = 0;
  for (unsigned char c : v)
    n = (n << 8) | c;
  return n;
}

inline std::vector<std::string> split_filter_list(const std::string &s) {
  std::vector<std::string> out;
  int depth = 0;
  std::size_t start = std::string::npos;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '(') {
      if (depth == 0)
        start = i;
      ++depth;
    } else if (s[i] == ')') {
      --depth;
      if (depth == 0 && start != std::string::npos) {
        out.push_back(s.substr(start, i - start + 1));
        start = std::string::npos;
      }
    }
  }
  if (out.empty() && !s.empty())
    out.push_back(s);
  return out;
}

inline std::string encode_filter(std::string f) {
  f = trim(f);
  if (f.size() >= 2 && f.front() == '(' && f.back() == ')')
    f = trim(f.substr(1, f.size() - 2));
  if (f.empty())
    f = "objectClass=*";
  if (f.front() == '&' || f.front() == '|') {
    uint8_t tag = f.front() == '&' ? 0xa0 : 0xa1;
    std::vector<std::string> subs;
    for (const auto &part : split_filter_list(f.substr(1)))
      subs.push_back(encode_filter(part));
    return ber_seq(subs, tag);
  }
  if (f.front() == '!') {
    return ber_tag(0xa2, encode_filter(f.substr(1)));
  }
  auto eq = f.find('=');
  if (eq == std::string::npos)
    return ber_octet(f, 0x87);
  std::string attr = trim(f.substr(0, eq));
  std::string val = f.substr(eq + 1);
  if (val == "*")
    return ber_octet(attr, 0x87);
  if (val.find('*') != std::string::npos) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (true) {
      auto star = val.find('*', pos);
      if (star == std::string::npos) {
        parts.push_back(val.substr(pos));
        break;
      }
      parts.push_back(val.substr(pos, star - pos));
      pos = star + 1;
    }
    std::vector<std::string> sub_items;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (parts[i].empty())
        continue;
      uint8_t stag = (i == 0) ? 0x80 : (i == parts.size() - 1 ? 0x82 : 0x81);
      sub_items.push_back(ber_octet(parts[i], stag));
    }
    return ber_seq({ber_octet(attr), ber_seq(sub_items)}, 0xa4);
  }
  return ber_seq({ber_octet(attr), ber_octet(val)}, 0xa3);
}

inline std::string url_decode(const std::string &in) {
  std::string out;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
      int v = std::stoi(in.substr(i + 1, 2), nullptr, 16);
      out.push_back(static_cast<char>(v));
      i += 2;
    } else if (in[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

struct ParsedLdapUrl {
  std::string host = "127.0.0.1";
  int port = 389;
  std::string path_dn;
  std::vector<std::string> attrs;
  std::string scope = "base";
  std::string filter = "(objectClass=*)";
  bool is_search_endpoint = false;
  bool is_ping_endpoint = false;
  bool is_bind_endpoint = false;
};

inline ParsedLdapUrl parse_ldap_url(const std::string &target) {
  ParsedLdapUrl u;
  std::string rest = target.substr(7); // after "ldap://"
  auto slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string tail = slash == std::string::npos ? "" : rest.substr(slash + 1);
  auto colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    u.host = hostport.substr(0, colon);
    u.port = std::atoi(hostport.substr(colon + 1).c_str());
  } else if (!hostport.empty()) {
    u.host = hostport;
  }
  auto qmark = tail.find('?');
  std::string raw_path = qmark == std::string::npos ? tail : tail.substr(0, qmark);
  std::string query = qmark == std::string::npos ? "" : tail.substr(qmark + 1);

  if (raw_path.empty() || raw_path == "ping") {
    u.is_ping_endpoint = (raw_path == "ping" || (raw_path.empty() && query.empty()));
  } else if (raw_path == "bind") {
    u.is_bind_endpoint = true;
  } else if (raw_path == "search" || raw_path == "entries") {
    u.is_search_endpoint = true;
    u.scope = "sub";
  } else if (raw_path.rfind("entry/", 0) == 0) {
    u.path_dn = url_decode(raw_path.substr(6));
    u.scope = "base";
  } else {
    u.path_dn = url_decode(raw_path);
  }

  if (!query.empty()) {
    if (query.find('=') != std::string::npos && query.find('?') == std::string::npos &&
        query.front() != '(') {
      // HTTP-style query string ?base=...&filter=...&scope=...
      u.is_search_endpoint = true;
      u.scope = "sub";
      std::size_t pos = 0;
      while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        auto eq = kv.find('=');
        if (eq != std::string::npos) {
          std::string k = url_decode(kv.substr(0, eq));
          std::string v = url_decode(kv.substr(eq + 1));
          if (k == "base")
            u.path_dn = v;
          else if (k == "filter")
            u.filter = v;
          else if (k == "scope")
            u.scope = v;
        }
        if (amp == std::string::npos)
          break;
        pos = amp + 1;
      }
    } else {
      // RFC 4516 ?attributes?scope?filter
      u.is_search_endpoint = true;
      std::vector<std::string> parts;
      std::size_t pos = 0;
      while (true) {
        auto q = query.find('?', pos);
        if (q == std::string::npos) {
          parts.push_back(query.substr(pos));
          break;
        }
        parts.push_back(query.substr(pos, q - pos));
        pos = q + 1;
      }
      if (!parts.empty() && !parts[0].empty()) {
        std::istringstream ss(parts[0]);
        std::string a;
        while (std::getline(ss, a, ','))
          if (!a.empty())
            u.attrs.push_back(url_decode(a));
      }
      if (parts.size() >= 2 && !parts[1].empty())
        u.scope = url_decode(parts[1]);
      else
        u.scope = "sub";
      if (parts.size() >= 3 && !parts[2].empty())
        u.filter = url_decode(parts[2]);
    }
  }
  return u;
}

inline bool send_all(int fd, const std::string &data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0)
      return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

inline bool recv_message(int fd, std::string &buf, BerNode &msg) {
  while (true) {
    msg = read_ber_node(buf, 0);
    if (!msg.needed) {
      buf.erase(0, msg.next);
      return true;
    }
    char chunk[4096];
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0)
      return false;
    buf.append(chunk, static_cast<std::size_t>(n));
  }
}

inline Response perform_ldap(Runtime *runtime, const Request &request, const std::string &target) {
  auto started = Clock::now();
  Response resp;
  resp.runtime = runtime;
  resp.url = target;
  resp.attempts = 1;
  resp.headers["content-type"] = "application/json";

  ParsedLdapUrl u = parse_ldap_url(target);
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    resp.error = "socket creation failed";
    if (!request.allow_error)
      throw dslvm::Error("HTTP transport failed: " + resp.error);
    return resp;
  }
  struct FdGuard {
    int f;
    ~FdGuard() {
      if (f >= 0)
        ::close(f);
    }
  } guard{fd};

  struct timeval tv;
  tv.tv_sec = request.timeout_ms / 1000;
  tv.tv_usec = (request.timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_port = htons(static_cast<uint16_t>(u.port));
  if (inet_pton(AF_INET, u.host.c_str(), &sin.sin_addr) <= 0) {
    struct hostent *he = gethostbyname(u.host.c_str());
    if (!he || !he->h_addr_list[0]) {
      resp.error = "unable to resolve LDAP host";
      if (!request.allow_error)
        throw dslvm::Error("HTTP transport failed: " + resp.error);
      return resp;
    }
    std::memcpy(&sin.sin_addr, he->h_addr_list[0], sizeof(sin.sin_addr));
  }

  if (::connect(fd, reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin)) < 0) {
    resp.error = "LDAP connection failed";
    if (!request.allow_error)
      throw dslvm::Error("HTTP transport failed: " + resp.error);
    return resp;
  }
  resp.connect_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();

  int64_t msg_id = 1;
  std::string rx_buf;

  auto do_bind = [&](const std::string &dn, const std::string &pw, int64_t &code_out,
                     std::string &diag_out) -> bool {
    std::string bind_op = ber_seq({ber_int(3), ber_octet(dn), ber_octet(pw, 0x80)}, 0x60);
    std::string pkt = ber_seq({ber_int(msg_id++), bind_op});
    if (!send_all(fd, pkt))
      return false;
    BerNode reply;
    if (!recv_message(fd, rx_buf, reply))
      return false;
    auto parts = read_ber_children(reply.value);
    if (parts.size() < 2)
      return false;
    auto rparts = read_ber_children(parts[1].value);
    code_out = rparts.empty() ? 80 : decode_ber_int(rparts[0].value);
    diag_out = rparts.size() > 2 ? rparts[2].value : "";
    return true;
  };

  std::string bind_dn = request.auth_user;
  std::string bind_pw = request.auth_password;
  if (u.is_bind_endpoint && !request.body.empty()) {
    auto j = parse_json(request.body, false);
    if (j.is_object()) {
      if (j.contains("dn") && j["dn"].is_string())
        bind_dn = j["dn"].get<std::string>();
      if (j.contains("password") && j["password"].is_string()) {
        bind_pw = j["password"].get<std::string>();
        runtime->secret(bind_pw);
      }
    }
  }

  if (u.is_bind_endpoint) {
    int64_t code = 0;
    std::string diag;
    if (!do_bind(bind_dn, bind_pw, code, diag)) {
      resp.error = "LDAP bind exchange failed";
      if (!request.allow_error)
        throw dslvm::Error("HTTP transport failed: " + resp.error);
      return resp;
    }
    resp.headers["x-ldap-result-code"] = std::to_string(code);
    if (code == 0) {
      resp.status = 200;
      Json out = {{"ok", true}, {"result", {{"bound", true}, {"dn", bind_dn}}}};
      resp.body = out.dump();
    } else {
      resp.status = (code == 49) ? 401 : 400;
      Json out = {{"ok", false}, {"error", diag.empty() ? "Invalid credentials" : diag}};
      resp.body = out.dump();
    }
  } else {
    if (!bind_dn.empty()) {
      int64_t bcode = 0;
      std::string bdiag;
      if (!do_bind(bind_dn, bind_pw, bcode, bdiag) || bcode != 0) {
        resp.status = 401;
        resp.headers["x-ldap-result-code"] = std::to_string(bcode);
        Json out = {{"ok", false}, {"error", bdiag.empty() ? "LDAP bind failed" : bdiag}};
        resp.body = out.dump();
        resp.size = static_cast<int64_t>(resp.body.size());
        return resp;
      }
    }

    if (request.verb == "GET" || request.verb == "HEAD") {
      int64_t scope_val = (u.scope == "base") ? 0 : (u.scope == "one" ? 1 : 2);
      std::vector<std::string> attr_items;
      for (const auto &a : u.attrs)
        attr_items.push_back(ber_octet(a));
      std::string search_op = ber_seq(
          {ber_octet(u.path_dn), ber_enum(scope_val), ber_enum(0), ber_int(0), ber_int(0),
           ber_bool(false), encode_filter(u.filter), ber_seq(attr_items)},
          0x63);
      std::string pkt = ber_seq({ber_int(msg_id++), search_op});
      if (!send_all(fd, pkt)) {
        resp.error = "LDAP search send failed";
        if (!request.allow_error)
          throw dslvm::Error("HTTP transport failed: " + resp.error);
        return resp;
      }
      Json entries = Json::array();
      int64_t done_code = 0;
      std::string done_diag;
      while (true) {
        BerNode reply;
        if (!recv_message(fd, rx_buf, reply)) {
          resp.error = "LDAP search receive failed";
          if (!request.allow_error)
            throw dslvm::Error("HTTP transport failed: " + resp.error);
          return resp;
        }
        auto parts = read_ber_children(reply.value);
        if (parts.size() < 2)
          continue;
        const auto &op = parts[1];
        if (op.tag == 0x64) {
          auto eparts = read_ber_children(op.value);
          std::string edn = eparts.empty() ? "" : eparts[0].value;
          Json attrs = Json::object();
          if (eparts.size() > 1) {
            for (const auto &anode : read_ber_children(eparts[1].value)) {
              auto kv = read_ber_children(anode.value);
              if (kv.size() >= 2) {
                std::string aname = kv[0].value;
                auto vnodes = read_ber_children(kv[1].value);
                if (vnodes.size() == 1)
                  attrs[aname] = vnodes[0].value;
                else {
                  Json arr = Json::array();
                  for (const auto &vn : vnodes)
                    arr.push_back(vn.value);
                  attrs[aname] = std::move(arr);
                }
              }
            }
          }
          entries.push_back({{"dn", edn}, {"attributes", std::move(attrs)}});
        } else if (op.tag == 0x65) {
          auto rparts = read_ber_children(op.value);
          done_code = rparts.empty() ? 0 : decode_ber_int(rparts[0].value);
          done_diag = rparts.size() > 2 ? rparts[2].value : "";
          break;
        }
      }
      resp.headers["x-ldap-result-code"] = std::to_string(done_code);
      if (u.is_ping_endpoint) {
        resp.status = (done_code == 0) ? 200 : 500;
        Json out = {{"ok", done_code == 0}, {"result", "PONG"}};
        resp.body = out.dump();
      } else if (done_code == 32) {
        resp.status = 404;
        Json out = {{"ok", false}, {"error", "No such object"}};
        resp.body = out.dump();
      } else if (done_code != 0) {
        resp.status = 400;
        Json out = {{"ok", false}, {"error", done_diag.empty() ? "LDAP search error" : done_diag}};
        resp.body = out.dump();
      } else if (!u.is_search_endpoint && u.scope == "base") {
        if (entries.empty()) {
          resp.status = 404;
          Json out = {{"ok", false}, {"error", "No such object"}};
          resp.body = out.dump();
        } else {
          resp.status = 200;
          Json out = {{"ok", true}, {"result", entries[0]}};
          resp.body = out.dump();
        }
      } else {
        resp.status = 200;
        Json out = {{"ok", true}, {"count", entries.size()}, {"result", entries}, {"entries", entries}};
        resp.body = out.dump();
      }
    } else if (request.verb == "PUT") {
      auto j = parse_json(request.body, true);
      if (!j.is_object())
        throw dslvm::Error("LDAP PUT body must be a JSON object of attributes");
      std::vector<std::string> attr_list;
      for (auto it = j.begin(); it != j.end(); ++it) {
        if (lower(it.key()) == "dn")
          continue;
        std::vector<std::string> vals;
        if (it.value().is_array()) {
          for (const auto &v : it.value())
            vals.push_back(ber_octet(v.is_string() ? v.get<std::string>() : v.dump()));
        } else if (it.value().is_string()) {
          vals.push_back(ber_octet(it.value().get<std::string>()));
        } else {
          vals.push_back(ber_octet(it.value().dump()));
        }
        attr_list.push_back(ber_seq({ber_octet(it.key()), ber_set(vals)}));
      }
      std::string add_op = ber_seq({ber_octet(u.path_dn), ber_seq(attr_list)}, 0x68);
      send_all(fd, ber_seq({ber_int(msg_id++), add_op}));
      BerNode reply;
      if (!recv_message(fd, rx_buf, reply))
        throw dslvm::Error("LDAP add receive failed");
      auto parts = read_ber_children(reply.value);
      auto rparts = parts.size() >= 2 ? read_ber_children(parts[1].value) : std::vector<BerNode>{};
      int64_t code = rparts.empty() ? 80 : decode_ber_int(rparts[0].value);
      std::string diag = rparts.size() > 2 ? rparts[2].value : "";
      resp.headers["x-ldap-result-code"] = std::to_string(code);
      if (code == 0) {
        resp.status = 200;
        resp.body = Json({{"ok", true}, {"result", "OK"}}).dump();
      } else {
        resp.status = (code == 68) ? 409 : 400;
        resp.body = Json({{"ok", false}, {"error", diag.empty() ? "LDAP add failed" : diag}}).dump();
      }
    } else if (request.verb == "POST" || request.verb == "PATCH") {
      auto j = parse_json(request.body, true);
      if (!j.is_object())
        throw dslvm::Error("LDAP POST body must be a JSON object of modifications");
      std::vector<std::string> changes;
      for (auto it = j.begin(); it != j.end(); ++it) {
        if (lower(it.key()) == "dn")
          continue;
        std::vector<std::string> vals;
        if (it.value().is_array()) {
          for (const auto &v : it.value())
            vals.push_back(ber_octet(v.is_string() ? v.get<std::string>() : v.dump()));
        } else if (!it.value().is_null()) {
          vals.push_back(
              ber_octet(it.value().is_string() ? it.value().get<std::string>() : it.value().dump()));
        }
        int64_t op_enum = vals.empty() ? 1 : 2;
        changes.push_back(
            ber_seq({ber_enum(op_enum), ber_seq({ber_octet(it.key()), ber_set(vals)})}));
      }
      std::string mod_op = ber_seq({ber_octet(u.path_dn), ber_seq(changes)}, 0x66);
      send_all(fd, ber_seq({ber_int(msg_id++), mod_op}));
      BerNode reply;
      if (!recv_message(fd, rx_buf, reply))
        throw dslvm::Error("LDAP modify receive failed");
      auto parts = read_ber_children(reply.value);
      auto rparts = parts.size() >= 2 ? read_ber_children(parts[1].value) : std::vector<BerNode>{};
      int64_t code = rparts.empty() ? 80 : decode_ber_int(rparts[0].value);
      std::string diag = rparts.size() > 2 ? rparts[2].value : "";
      resp.headers["x-ldap-result-code"] = std::to_string(code);
      if (code == 0) {
        resp.status = 200;
        resp.body = Json({{"ok", true}, {"result", "OK"}}).dump();
      } else {
        resp.status = (code == 32) ? 404 : 400;
        resp.body =
            Json({{"ok", false}, {"error", diag.empty() ? "LDAP modify failed" : diag}}).dump();
      }
    } else if (request.verb == "DELETE") {
      std::string del_op = ber_octet(u.path_dn, 0x4a);
      send_all(fd, ber_seq({ber_int(msg_id++), del_op}));
      BerNode reply;
      if (!recv_message(fd, rx_buf, reply))
        throw dslvm::Error("LDAP delete receive failed");
      auto parts = read_ber_children(reply.value);
      auto rparts = parts.size() >= 2 ? read_ber_children(parts[1].value) : std::vector<BerNode>{};
      int64_t code = rparts.empty() ? 80 : decode_ber_int(rparts[0].value);
      std::string diag = rparts.size() > 2 ? rparts[2].value : "";
      resp.headers["x-ldap-result-code"] = std::to_string(code);
      if (code == 0) {
        resp.status = 200;
        resp.body = Json({{"ok", true}, {"result", 1}}).dump();
      } else if (code == 32) {
        resp.status = 200;
        resp.body = Json({{"ok", true}, {"result", 0}}).dump();
      } else {
        resp.status = 400;
        resp.body =
            Json({{"ok", false}, {"error", diag.empty() ? "LDAP delete failed" : diag}}).dump();
      }
    } else {
      throw dslvm::Error("unsupported LDAP verb: " + request.verb);
    }
  }

  // Send UnbindRequest (0x42)
  std::string unbind_pkt = ber_seq({ber_int(msg_id++), ber_tag(0x42, "")});
  send_all(fd, unbind_pkt);

  auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  resp.total_ms = elapsed;
  resp.ttfb_ms = elapsed;
  resp.first_data_ms = elapsed;
  resp.size = static_cast<int64_t>(resp.body.size());
  return resp;
}

} // namespace lmcurl::ldap_wire
