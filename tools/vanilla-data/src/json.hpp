// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal JSON parser for the +MC vanilla-data tool.
// Deliberately tiny and dependency-free. Supports: null, bool, number (double),
// string (with escapes incl. \uXXXX and surrogate pairs), array, object.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pj {

class J {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  J() : type_(Type::Null) {}
  J(bool b) : type_(Type::Bool) {
    bool_ = b;
  }
  J(double n) : type_(Type::Number) {
    num_ = n;
  }
  J(const char* s) : type_(Type::String) {
    str_ = s;
  }
  J(std::string s) : type_(Type::String) {
    str_ = std::move(s);
  }

  Type type() const {
    return type_;
  }
  bool is_null() const {
    return type_ == Type::Null;
  }
  bool is_bool() const {
    return type_ == Type::Bool;
  }
  bool is_number() const {
    return type_ == Type::Number;
  }
  bool is_string() const {
    return type_ == Type::String;
  }
  bool is_array() const {
    return type_ == Type::Array;
  }
  bool is_object() const {
    return type_ == Type::Object;
  }

  bool as_bool() const {
    if (type_ != Type::Bool)
      throw std::runtime_error("json: not a bool");
    return bool_;
  }
  double as_number() const {
    if (type_ != Type::Number)
      throw std::runtime_error("json: not a number");
    return num_;
  }
  long long as_int() const {
    if (type_ != Type::Number)
      throw std::runtime_error("json: not a number");
    return static_cast<long long>(num_);
  }
  const std::string& as_string() const {
    if (type_ != Type::String)
      throw std::runtime_error("json: not a string");
    return str_;
  }

  // Object access. Linear lookup is fine for our sizes.
  bool has(const std::string& key) const {
    if (type_ != Type::Object)
      return false;
    for (const auto& kv : obj_)
      if (kv.first == key)
        return true;
    return false;
  }
  const J& at(const std::string& key) const {
    if (type_ != Type::Object)
      throw std::runtime_error("json: not an object");
    for (const auto& kv : obj_)
      if (kv.first == key)
        return kv.second;
    throw std::runtime_error("json: missing key '" + key + "'");
  }
  const J* find(const std::string& key) const {
    if (type_ != Type::Object)
      return nullptr;
    for (const auto& kv : obj_)
      if (kv.first == key)
        return &kv.second;
    return nullptr;
  }
  J* find(const std::string& key) {
    if (type_ != Type::Object)
      return nullptr;
    for (auto& kv : obj_)
      if (kv.first == key)
        return &kv.second;
    return nullptr;
  }
  const std::vector<std::pair<std::string, J>>& object_items() const {
    if (type_ != Type::Object)
      throw std::runtime_error("json: not an object");
    return obj_;
  }
  std::vector<std::pair<std::string, J>>& object_items() {
    if (type_ != Type::Object)
      throw std::runtime_error("json: not an object");
    return obj_;
  }
  const std::vector<J>& array_items() const {
    if (type_ != Type::Array)
      throw std::runtime_error("json: not an array");
    return arr_;
  }
  std::vector<J>& array_items() {
    if (type_ != Type::Array)
      throw std::runtime_error("json: not an array");
    return arr_;
  }

  void push_back(J v) {
    type_ = Type::Array;
    arr_.push_back(std::move(v));
  }
  void set(const std::string& key, J v) {
    type_ = Type::Object;
    for (auto& kv : obj_) {
      if (kv.first == key) {
        kv.second = std::move(v);
        return;
      }
    }
    obj_.emplace_back(key, std::move(v));
  }

  static J parse(const std::string& text) {
    size_t pos = 0;
    skip_ws(text, pos);
    J v = parse_value(text, pos);
    skip_ws(text, pos);
    if (pos != text.size())
      throw std::runtime_error("json: trailing content at " + std::to_string(pos));
    return v;
  }

private:
  Type type_;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  std::vector<J> arr_;
  std::vector<std::pair<std::string, J>> obj_;

  static void skip_ws(const std::string& s, size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
      ++p;
  }

  static char peek(const std::string& s, size_t p) {
    if (p >= s.size())
      throw std::runtime_error("json: unexpected end of input");
    return s[p];
  }

  static void expect(const std::string& s, size_t& p, char c) {
    if (p >= s.size() || s[p] != c)
      throw std::runtime_error(std::string("json: expected '") + c + "' at " + std::to_string(p));
    ++p;
  }

  static J parse_value(const std::string& s, size_t& p) {
    skip_ws(s, p);
    char c = peek(s, p);
    switch (c) {
    case '{':
      return parse_object(s, p);
    case '[':
      return parse_array(s, p);
    case '"':
      return J(parse_string(s, p));
    case 't':
      expect_lit(s, p, "true");
      return J(true);
    case 'f':
      expect_lit(s, p, "false");
      return J(false);
    case 'n':
      expect_lit(s, p, "null");
      return J();
    default:
      return J(parse_number(s, p));
    }
  }

  static void expect_lit(const std::string& s, size_t& p, const char* lit) {
    for (const char* q = lit; *q; ++q) {
      if (p >= s.size() || s[p] != *q)
        throw std::runtime_error(std::string("json: bad literal '") + lit + "' at " +
                                 std::to_string(p));
      ++p;
    }
  }

  static J parse_object(const std::string& s, size_t& p) {
    J obj;
    obj.type_ = Type::Object;
    expect(s, p, '{');
    skip_ws(s, p);
    if (p < s.size() && s[p] == '}') {
      ++p;
      return obj;
    }
    for (;;) {
      skip_ws(s, p);
      if (peek(s, p) != '"')
        throw std::runtime_error("json: expected key string at " + std::to_string(p));
      std::string key = parse_string(s, p);
      skip_ws(s, p);
      expect(s, p, ':');
      J val = parse_value(s, p);
      obj.obj_.emplace_back(std::move(key), std::move(val));
      skip_ws(s, p);
      char c = peek(s, p);
      if (c == ',') {
        ++p;
        continue;
      }
      if (c == '}') {
        ++p;
        return obj;
      }
      throw std::runtime_error("json: expected ',' or '}' at " + std::to_string(p));
    }
  }

  static J parse_array(const std::string& s, size_t& p) {
    J arr;
    arr.type_ = Type::Array;
    expect(s, p, '[');
    skip_ws(s, p);
    if (p < s.size() && s[p] == ']') {
      ++p;
      return arr;
    }
    for (;;) {
      J val = parse_value(s, p);
      arr.arr_.push_back(std::move(val));
      skip_ws(s, p);
      char c = peek(s, p);
      if (c == ',') {
        ++p;
        continue;
      }
      if (c == ']') {
        ++p;
        return arr;
      }
      throw std::runtime_error("json: expected ',' or ']' at " + std::to_string(p));
    }
  }

  static std::string parse_string(const std::string& s, size_t& p) {
    expect(s, p, '"');
    std::string out;
    while (p < s.size()) {
      unsigned char c = static_cast<unsigned char>(s[p]);
      if (c == '"') {
        ++p;
        return out;
      }
      if (c == '\\') {
        ++p;
        if (p >= s.size())
          throw std::runtime_error("json: bad escape");
        char e = s[p++];
        switch (e) {
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case '/':
          out += '/';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          uint32_t cp = parse_hex4(s, p);
          if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate
            if (p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u') {
              p += 2;
              uint32_t lo = parse_hex4(s, p);
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                throw std::runtime_error("json: bad low surrogate");
              }
            } else {
              throw std::runtime_error("json: lone high surrogate");
            }
          }
          append_utf8(out, cp);
          break;
        }
        default:
          throw std::runtime_error("json: unknown escape '\\" + std::string(1, e) + "'");
        }
      } else {
        out += static_cast<char>(c);
        ++p;
      }
    }
    throw std::runtime_error("json: unterminated string");
  }

  static uint32_t parse_hex4(const std::string& s, size_t& p) {
    if (p + 4 > s.size())
      throw std::runtime_error("json: bad \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s[p++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<uint32_t>(c - 'A' + 10);
      else
        throw std::runtime_error("json: bad hex digit");
    }
    return v;
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  static double parse_number(const std::string& s, size_t& p) {
    size_t start = p;
    if (p < s.size() && (s[p] == '-' || s[p] == '+'))
      ++p;
    bool any = false;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
      ++p;
      any = true;
    }
    if (p < s.size() && s[p] == '.') {
      ++p;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        ++p;
        any = true;
      }
    }
    if (!any)
      throw std::runtime_error("json: bad number at " + std::to_string(start));
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      ++p;
      if (p < s.size() && (s[p] == '-' || s[p] == '+'))
        ++p;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9')
        ++p;
    }
    return std::stod(s.substr(start, p - start));
  }
};

} // namespace pj
