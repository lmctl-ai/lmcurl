// cpp-dsl-sdk: the C++ runtime for lmscript `_dsl_` bytecode. Header-only, C++17, standard library only.
// Single source of truth: github.com/lmctlhq/cpp-dsl-sdk/include/dslvm.hpp (hosts vendor it by pinned sha256, see README).
//
// Public API (everything else in this header is an implementation detail):
//   dslvm::Error                         exception type for load, verify and run-time failures (what() names the problem)
//   dslvm::Value                         tagged value: VOID, INT (int32), FLOAT (binary64), STR (UTF-8), BOOL, OBJ (host pointer + schema type name)
//   dslvm::Args                          std::vector<Value> of call arguments, in order
//   dslvm::Registry                      what the host offers, by schema name: function(name, argc, fn), method(type, name, argc, fn),
//                                        context(name, type) declares a context variable's type
//   dslvm::Context                       the live objects of one run: set(name, pointer)
//   dslvm::Program::load(bytes, schema, registry)  parses, binds and verifies; throws before anything runs
//   dslvm::Program::run(context, registry)         executes the chunk once; throws dslvm::Error on a run-time failure
//   dslvm::VERSION                       the bytecode format version this SDK reads (and nothing else: no backward compatibility)
//   dslvm::sha256 / hex / read_file      helpers the hosts use for the schema binding and files
//
// Bytecode v3 (big-endian, fixed widths, IEEE-754 binary64): magic "DSLB", u16 version, sha256(schema bytes), u16 string
// constants (u16 length + UTF-8), u16 locals, u32 code length, code. Opcodes: enum Op below. The living specification is
// lmctl-mono lmscript/durable-memory/design/dsl-language.md; the compiler is lmscript's dsl.ts.
//
// Guarantees of load(): wrong magic, other version, schema-hash mismatch, truncation, trailing bytes, undecodable
// instructions, out-of-range constants/locals, jumps that do not land on an instruction start or go backwards (so every
// program terminates: the language has no loops), missing HALT, undeclared context names, unbound functions/methods and
// wrong arities are all refused with a message. run() additionally bounds the operand stack (MAX_STACK) and the size of
// concatenated strings (MAX_STRING_BYTES) and checks value kinds defensively.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dslvm {

struct Error : std::runtime_error { explicit Error(const std::string& m) : std::runtime_error(m) {} };

// ---- SHA-256 (FIPS 180-4), used only to bind the bytecode to the exact schema bytes ----
inline std::string sha256(const std::string& data) {
  static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  std::string msg = data;
  uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back(0);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data() + off + i * 4);
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
      uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
      uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22), maj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
  }
  std::string out(32, '\0');
  for (int i = 0; i < 8; ++i) for (int j = 0; j < 4; ++j) out[i*4+j] = static_cast<char>((h[i] >> (24 - j*8)) & 0xff);
  return out;
}

inline std::string hex(const std::string& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) { out.push_back(digits[c >> 4]); out.push_back(digits[c & 15]); }
  return out;
}

// ---- Values ----
struct Value {
  enum Kind { VOID, INT, FLOAT, STR, BOOL, OBJ } kind = VOID;
  int32_t i = 0; double f = 0.0; std::string s; bool b = false; void* obj = nullptr; std::string type;  // type: schema type name of obj
  static Value none() { return Value(); }
  static Value integer(int32_t v) { Value x; x.kind = INT; x.i = v; return x; }
  static Value real(double v) { Value x; x.kind = FLOAT; x.f = v; return x; }
  static Value string(std::string v) { Value x; x.kind = STR; x.s = std::move(v); return x; }
  static Value boolean(bool v) { Value x; x.kind = BOOL; x.b = v; return x; }
  static Value object(const std::string& type, void* p) { Value x; x.kind = OBJ; x.obj = p; x.type = type; return x; }
  const char* kind_name() const { static const char* n[] = {"void","int","float","string","bool","object"}; return n[kind]; }
};
using Args = std::vector<Value>;
using Function = std::function<Value(Args&)>;
using Method = std::function<Value(void* self, Args&)>;

// ---- Host registry: what the host offers, by schema name ----
class Registry {
 public:
  void function(const std::string& name, size_t argc, Function fn) { functions_[name] = {argc, std::move(fn)}; }
  void method(const std::string& type, const std::string& name, size_t argc, Method fn) { methods_[type + "." + name] = {argc, std::move(fn)}; }
  void context(const std::string& name, const std::string& type) { context_[name] = type; }
  const std::pair<size_t, Function>* find_function(const std::string& name) const { auto it = functions_.find(name); return it == functions_.end() ? nullptr : &it->second; }
  const std::pair<size_t, Method>* find_method(const std::string& type, const std::string& name) const { auto it = methods_.find(type + "." + name); return it == methods_.end() ? nullptr : &it->second; }
  const std::string* context_type(const std::string& name) const { auto it = context_.find(name); return it == context_.end() ? nullptr : &it->second; }
 private:
  std::map<std::string, std::pair<size_t, Function>> functions_;
  std::map<std::string, std::pair<size_t, Method>> methods_;
  std::map<std::string, std::string> context_;
};

// ---- Per-run context: the host's live objects, by declared name ----
class Context {
 public:
  void set(const std::string& name, void* object) { objects_[name] = object; }
  void* get(const std::string& name) const { auto it = objects_.find(name); return it == objects_.end() ? nullptr : it->second; }
 private:
  std::map<std::string, void*> objects_;
};

// ---- Bytecode ----
enum Op : uint8_t {
  PUSH_INT = 0x01, PUSH_STR = 0x02, PUSH_BOOL = 0x03, LOAD_LOCAL = 0x04, STORE_LOCAL = 0x05, LOAD_CTX = 0x06,
  CALL_FN = 0x07, CALL_METHOD = 0x08, EQ = 0x09, NE = 0x0A, NOT = 0x0B, JUMP = 0x0C, JUMP_IF_FALSE = 0x0D,
  JUMP_IF_TRUE = 0x0E, DUP = 0x0F, POP = 0x10, HALT = 0x11,
  // v2: floats, typed arithmetic and ordering, explicit widening
  PUSH_FLOAT = 0x12, ADD_I = 0x13, SUB_I = 0x14, MUL_I = 0x15, DIV_I = 0x16, ADD_F = 0x17, SUB_F = 0x18, MUL_F = 0x19, DIV_F = 0x1A,
  NEG_I = 0x1B, NEG_F = 0x1C, LT_I = 0x1D, LE_I = 0x1E, GT_I = 0x1F, GE_I = 0x20, LT_F = 0x21, LE_F = 0x22, GT_F = 0x23, GE_F = 0x24, I2F = 0x25,
  // v3: string concatenation and str() conversions
  CONCAT = 0x26, I2S = 0x27, F2S = 0x28, B2S = 0x29
};
static const char MAGIC[4] = {'D', 'S', 'L', 'B'};
static const uint16_t VERSION = 3;
static const size_t MAX_STACK = 1024;                       // operand stack depth per run
static const size_t MAX_STRING_BYTES = 16u * 1024u * 1024u; // result size of one CONCAT

class Reader {
 public:
  explicit Reader(const std::string& bytes) : b_(&bytes) {}
  void seek(size_t pos) { if (pos > b_->size()) throw Error("jump target " + std::to_string(pos) + " is outside the code"); pos_ = pos; }
  uint8_t u8() { need(1); return static_cast<uint8_t>((*b_)[pos_++]); }
  uint16_t u16() { need(2); uint16_t v = (uint16_t(uint8_t((*b_)[pos_])) << 8) | uint8_t((*b_)[pos_+1]); pos_ += 2; return v; }
  uint32_t u32() { need(4); uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | uint8_t((*b_)[pos_+i]); pos_ += 4; return v; }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  double f64() { need(8); uint64_t bits = 0; for (int i = 0; i < 8; ++i) bits = (bits << 8) | uint8_t((*b_)[pos_+i]); pos_ += 8; double d; std::memcpy(&d, &bits, 8); return d; }
  std::string bytes(size_t n) { need(n); std::string s = b_->substr(pos_, n); pos_ += n; return s; }
  size_t pos() const { return pos_; }
  bool done() const { return pos_ == b_->size(); }
 private:
  void need(size_t n) { if (pos_ + n > b_->size()) throw Error("bytecode is truncated: wanted " + std::to_string(n) + " byte(s) at offset " + std::to_string(pos_) + ", file has " + std::to_string(b_->size())); }
  const std::string* b_; size_t pos_ = 0;
};

class Program {
 public:
  // Parses, binds and verifies. Nothing executes here. Throws dslvm::Error with a message naming the problem.
  static Program load(const std::string& bytecode, const std::string& schemaBytes, const Registry& reg) {
    Program p;
    Reader r(bytecode);
    if (bytecode.size() < 4 || std::memcmp(bytecode.data(), MAGIC, 4) != 0) throw Error("not a DSL bytecode file: bad magic (expected 'DSLB')");
    r.bytes(4);
    uint16_t version = r.u16();
    if (version != VERSION) throw Error("unsupported bytecode version " + std::to_string(version) + " (this runtime reads version " + std::to_string(VERSION) + ")");
    std::string hash = r.bytes(32), expected = sha256(schemaBytes);
    if (hash != expected) throw Error("schema mismatch: bytecode was compiled against schema sha256 " + hex(hash) + ", the given schema has sha256 " + hex(expected));
    uint16_t nconst = r.u16();
    for (uint16_t i = 0; i < nconst; ++i) { uint16_t len = r.u16(); p.consts_.push_back(r.bytes(len)); }
    p.nlocals_ = r.u16();
    uint32_t codeLen = r.u32();
    p.code_ = r.bytes(codeLen);
    if (!r.done()) throw Error("bytecode has " + std::to_string(bytecode.size() - r.pos()) + " trailing byte(s) after the code section");
    p.verify(reg);
    return p;
  }

  // Executes the chunk once. The host's objects come from ctx (declared names only).
  void run(const Context& ctx, const Registry& reg) const {
    std::vector<Value> stack, locals(nlocals_);
    Reader r(code_);
    auto pop = [&]() { if (stack.empty()) throw Error("stack underflow"); Value v = std::move(stack.back()); stack.pop_back(); return v; };
    auto push = [&](Value v) { if (stack.size() >= MAX_STACK) throw Error("operand stack exceeds " + std::to_string(MAX_STACK) + " values"); stack.push_back(std::move(v)); };
    auto jump = [&](int32_t rel) { int64_t target = int64_t(r.pos()) + rel; if (target < 0) throw Error("jump before the code start"); r.seek(size_t(target)); };
    while (true) {
      uint8_t op = r.u8();
      switch (op) {
        case PUSH_INT: push(Value::integer(r.i32())); break;
        case PUSH_FLOAT: push(Value::real(r.f64())); break;
        case PUSH_STR: push(Value::string(consts_[r.u16()])); break;
        case PUSH_BOOL: push(Value::boolean(r.u8() != 0)); break;
        case LOAD_LOCAL: {
          uint16_t s = r.u16();
          if (locals[s].kind == Value::VOID) throw Error("local slot " + std::to_string(s) + " read before assignment");
          push(locals[s]); break;
        }
        case STORE_LOCAL: { uint16_t s = r.u16(); locals[s] = pop(); break; }
        case LOAD_CTX: {
          const std::string& name = consts_[r.u16()];
          void* obj = ctx.get(name);
          if (!obj) throw Error("context variable '" + name + "' was not provided by the host for this run");
          push(Value::object(*reg.context_type(name), obj)); break;
        }
        case CALL_FN: {
          const std::string& name = consts_[r.u16()]; uint8_t argc = r.u8();
          Args args(argc); for (int i = argc - 1; i >= 0; --i) args[i] = pop();
          Value result = reg.find_function(name)->second(args);
          if (result.kind != Value::VOID) push(std::move(result));
          break;
        }
        case CALL_METHOD: {
          const std::string& type = consts_[r.u16()]; const std::string& name = consts_[r.u16()]; uint8_t argc = r.u8();
          Args args(argc); for (int i = argc - 1; i >= 0; --i) args[i] = pop();
          Value self = pop();
          if (self.kind != Value::OBJ || self.type != type) throw Error("method " + type + "." + name + " called on a " + (self.kind == Value::OBJ ? self.type : std::string(self.kind_name())));
          Value result = reg.find_method(type, name)->second(self.obj, args);
          if (result.kind != Value::VOID) push(std::move(result));
          break;
        }
        case EQ: case NE: {
          Value b = pop(), a = pop();
          if (a.kind != b.kind) throw Error(std::string("cannot compare ") + a.kind_name() + " with " + b.kind_name());
          bool eq = a.kind == Value::INT ? a.i == b.i : a.kind == Value::FLOAT ? a.f == b.f : a.kind == Value::STR ? a.s == b.s : a.kind == Value::BOOL ? a.b == b.b : a.obj == b.obj;
          push(Value::boolean(op == EQ ? eq : !eq)); break;
        }
        case ADD_I: case SUB_I: case MUL_I: case DIV_I: case LT_I: case LE_I: case GT_I: case GE_I: {
          Value b = pop(), a = pop();
          if (a.kind != Value::INT || b.kind != Value::INT) throw Error(std::string("int operator applied to ") + a.kind_name() + " and " + b.kind_name());
          int64_t x = a.i, y = b.i, r64 = 0;
          switch (op) {
            case ADD_I: r64 = x + y; break; case SUB_I: r64 = x - y; break; case MUL_I: r64 = x * y; break;
            case DIV_I: if (y == 0) throw Error("int division by zero"); r64 = x / y; break;  // truncates toward zero
            case LT_I: push(Value::boolean(x < y)); continue; case LE_I: push(Value::boolean(x <= y)); continue;
            case GT_I: push(Value::boolean(x > y)); continue; default: push(Value::boolean(x >= y)); continue;
          }
          if (r64 > INT32_MAX || r64 < INT32_MIN) throw Error("int overflow: " + std::to_string(r64) + " does not fit a 32-bit int");
          push(Value::integer(int32_t(r64))); break;
        }
        case ADD_F: case SUB_F: case MUL_F: case DIV_F: case LT_F: case LE_F: case GT_F: case GE_F: {
          Value b = pop(), a = pop();
          if (a.kind != Value::FLOAT || b.kind != Value::FLOAT) throw Error(std::string("float operator applied to ") + a.kind_name() + " and " + b.kind_name());
          switch (op) {
            case ADD_F: push(Value::real(a.f + b.f)); break; case SUB_F: push(Value::real(a.f - b.f)); break;
            case MUL_F: push(Value::real(a.f * b.f)); break; case DIV_F: push(Value::real(a.f / b.f)); break;  // IEEE: inf/nan allowed
            case LT_F: push(Value::boolean(a.f < b.f)); break; case LE_F: push(Value::boolean(a.f <= b.f)); break;
            case GT_F: push(Value::boolean(a.f > b.f)); break; default: push(Value::boolean(a.f >= b.f)); break;
          }
          break;
        }
        case NEG_I: { Value a = pop(); if (a.kind != Value::INT) throw Error(std::string("int negation applied to ") + a.kind_name()); if (a.i == INT32_MIN) throw Error("int overflow: negating the smallest int"); push(Value::integer(-a.i)); break; }
        case NEG_F: { Value a = pop(); if (a.kind != Value::FLOAT) throw Error(std::string("float negation applied to ") + a.kind_name()); push(Value::real(-a.f)); break; }
        case I2F: { Value a = pop(); if (a.kind != Value::INT) throw Error(std::string("float() applied to ") + a.kind_name()); push(Value::real(double(a.i))); break; }
        case CONCAT: {
          Value b = pop(), a = pop();
          if (a.kind != Value::STR || b.kind != Value::STR) throw Error(std::string("string + applied to ") + a.kind_name() + " and " + b.kind_name());
          if (a.s.size() + b.s.size() > MAX_STRING_BYTES) throw Error("string + result exceeds " + std::to_string(MAX_STRING_BYTES) + " bytes");
          push(Value::string(a.s + b.s)); break;
        }
        case I2S: { Value a = pop(); if (a.kind != Value::INT) throw Error(std::string("str(int) applied to ") + a.kind_name()); push(Value::string(std::to_string(a.i))); break; }
        case F2S: {  // %.15g: the same text poc-numpy prints; deterministic across hosts
          Value a = pop(); if (a.kind != Value::FLOAT) throw Error(std::string("str(float) applied to ") + a.kind_name());
          char buf[64]; std::snprintf(buf, sizeof buf, "%.15g", a.f); push(Value::string(buf)); break;
        }
        case B2S: { Value a = pop(); if (a.kind != Value::BOOL) throw Error(std::string("str(bool) applied to ") + a.kind_name()); push(Value::string(a.b ? "true" : "false")); break; }
        case NOT: {
          Value a = pop();
          if (a.kind != Value::BOOL) throw Error(std::string("! applied to a ") + a.kind_name());
          push(Value::boolean(!a.b)); break;
        }
        case JUMP: jump(r.i32()); break;
        case JUMP_IF_FALSE: case JUMP_IF_TRUE: {
          int32_t rel = r.i32(); Value c = pop();
          if (c.kind != Value::BOOL) throw Error(std::string("condition is a ") + c.kind_name() + ", not a bool");
          if (c.b == (op == JUMP_IF_TRUE)) jump(rel);
          break;
        }
        case DUP: {
          if (stack.empty()) throw Error("stack underflow");
          push(stack.back()); break;
        }
        case POP: pop(); break;
        case HALT: return;
        default: throw Error("unknown opcode " + std::to_string(op));
      }
    }
  }

  size_t constant_count() const { return consts_.size(); }
  size_t code_size() const { return code_.size(); }

 private:
  // Static verification: every instruction decodes, every index is in range, every jump lands on an instruction start,
  // every referenced name is bound in the registry with the same arity, the code ends with HALT.
  void verify(const Registry& reg) const {
    std::vector<size_t> starts; std::vector<std::pair<size_t, int64_t>> jumps;
    Reader r(code_);
    auto cidx = [&](uint16_t i) -> const std::string& { if (i >= consts_.size()) throw Error("constant index " + std::to_string(i) + " out of range (" + std::to_string(consts_.size()) + " constants)"); return consts_[i]; };
    bool halted = false;
    while (!r.done()) {
      starts.push_back(r.pos());
      uint8_t op = r.u8(); halted = false;
      switch (op) {
        case PUSH_INT: r.i32(); break;
        case PUSH_FLOAT: r.f64(); break;
        case PUSH_STR: cidx(r.u16()); break;
        case PUSH_BOOL: {
          uint8_t v = r.u8();
          if (v > 1) throw Error("PUSH_BOOL operand must be 0 or 1");
          break;
        }
        case LOAD_LOCAL: case STORE_LOCAL: {
          uint16_t s = r.u16();
          if (s >= nlocals_) throw Error("local slot " + std::to_string(s) + " out of range (" + std::to_string(nlocals_) + " locals)");
          break;
        }
        case LOAD_CTX: {
          const std::string& n = cidx(r.u16());
          if (!reg.context_type(n)) throw Error("context variable '" + n + "' is not declared by the host");
          break;
        }
        case CALL_FN: {
          const std::string& n = cidx(r.u16()); uint8_t argc = r.u8(); auto f = reg.find_function(n);
          if (!f) throw Error("host function '" + n + "' is not bound");
          if (f->first != argc) throw Error("host function '" + n + "' takes " + std::to_string(f->first) + " argument(s), bytecode passes " + std::to_string(argc));
          break;
        }
        case CALL_METHOD: {
          const std::string& t = cidx(r.u16()); const std::string& n = cidx(r.u16()); uint8_t argc = r.u8(); auto m = reg.find_method(t, n);
          if (!m) throw Error("method '" + t + "." + n + "' is not bound");
          if (m->first != argc) throw Error("method '" + t + "." + n + "' takes " + std::to_string(m->first) + " argument(s), bytecode passes " + std::to_string(argc));
          break;
        }
        case EQ: case NE: case NOT: case DUP: case POP: break;
        case ADD_I: case SUB_I: case MUL_I: case DIV_I: case ADD_F: case SUB_F: case MUL_F: case DIV_F: case NEG_I: case NEG_F:
        case LT_I: case LE_I: case GT_I: case GE_I: case LT_F: case LE_F: case GT_F: case GE_F: case I2F: case CONCAT: case I2S: case F2S: case B2S: break;
        case JUMP: case JUMP_IF_FALSE: case JUMP_IF_TRUE: {
          int32_t rel = r.i32();
          if (rel < 0) throw Error("jump at offset " + std::to_string(starts.back()) + " goes backwards (the language has no loops; every jump must move forward)");
          jumps.emplace_back(r.pos(), int64_t(r.pos()) + rel); break;
        }
        case HALT: halted = true; break;
        default: throw Error("unknown opcode " + std::to_string(op) + " at offset " + std::to_string(starts.back()));
      }
    }
    if (!halted) throw Error("code does not end with HALT");
    if (nlocals_ > 4096) throw Error("too many locals: " + std::to_string(nlocals_) + " (limit 4096)");
    for (auto& j : jumps) {
      bool ok = j.second >= 0 && std::find(starts.begin(), starts.end(), size_t(j.second)) != starts.end();
      if (!ok) throw Error("jump at offset " + std::to_string(j.first) + " targets " + std::to_string(j.second) + ", not an instruction start");
    }
  }
  std::vector<std::string> consts_; uint16_t nlocals_ = 0; std::string code_;
};

inline std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Error("cannot read " + path);
  std::ostringstream s; s << in.rdbuf(); return s.str();
}

}  // namespace dslvm
