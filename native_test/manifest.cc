#include "manifest.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace wt_test {
namespace {

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : s_(text) {}

  bool Document(Json* out, std::string* error) {
    if (!Value(out, 0) || (SkipSpace(), pos_ != s_.size())) {
      *error = "JSON error near byte " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  void SkipSpace() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' ||
                                s_[pos_] == '\r' || s_[pos_] == '\t')) {
      ++pos_;
    }
  }

  bool Eat(char c) {
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool Literal(const char* word) {
    const std::string w(word);
    if (s_.compare(pos_, w.size(), w) != 0) return false;
    pos_ += w.size();
    return true;
  }

  bool String(std::string* out) {
    if (!Eat('"')) return false;
    out->clear();
    while (pos_ < s_.size() && s_[pos_] != '"') {
      char c = s_[pos_++];
      if (c == '\\') {
        if (pos_ >= s_.size()) return false;
        c = s_[pos_++];
        switch (c) {
          case 'n':
            c = '\n';
            break;
          case 't':
            c = '\t';
            break;
          case 'r':
            c = '\r';
            break;
          case 'b':
            c = '\b';
            break;
          case 'f':
            c = '\f';
            break;
          case 'u':
            return false;  // The manifest is ASCII; not needed.
          default:
            break;  // \" \\ \/
        }
      }
      out->push_back(c);
    }
    return Eat('"');
  }

  bool Value(Json* out, int depth) {
    if (depth > 32) return false;
    SkipSpace();
    if (pos_ >= s_.size()) return false;
    const char c = s_[pos_];
    if (c == '{') {
      ++pos_;
      out->type = Json::Type::kObject;
      if (Eat('}')) return true;
      do {
        std::pair<std::string, Json> member;
        if (!String(&member.first) || !Eat(':') ||
            !Value(&member.second, depth + 1)) {
          return false;
        }
        out->object.push_back(std::move(member));
      } while (Eat(','));
      return Eat('}');
    }
    if (c == '[') {
      ++pos_;
      out->type = Json::Type::kArray;
      if (Eat(']')) return true;
      do {
        out->array.emplace_back();
        if (!Value(&out->array.back(), depth + 1)) return false;
      } while (Eat(','));
      return Eat(']');
    }
    if (c == '"') {
      out->type = Json::Type::kString;
      return String(&out->string);
    }
    if (Literal("true")) {
      out->type = Json::Type::kBool;
      out->boolean = true;
      return true;
    }
    if (Literal("false")) {
      out->type = Json::Type::kBool;
      return true;
    }
    if (Literal("null")) return true;
    const char* begin = s_.c_str() + pos_;
    char* end = nullptr;
    out->number = std::strtod(begin, &end);
    if (end == begin) return false;
    out->type = Json::Type::kNumber;
    pos_ += static_cast<size_t>(end - begin);
    return true;
  }

  const std::string& s_;
  size_t pos_ = 0;
};

}  // namespace

const Json& Json::operator[](const std::string& key) const {
  static const Json kNull;
  for (const auto& member : object) {
    if (member.first == key) return member.second;
  }
  return kNull;
}

bool Json::has(const std::string& key) const {
  for (const auto& member : object) {
    if (member.first == key) return true;
  }
  return false;
}

int64_t Json::Int() const {
  return type == Type::kNumber ? static_cast<int64_t>(std::llround(number)) : 0;
}

std::vector<int64_t> Json::Ints() const {
  std::vector<int64_t> out;
  for (const Json& v : array) out.push_back(v.Int());
  return out;
}

bool ParseJson(const std::string& text, Json* out, std::string* error) {
  *out = Json();
  return JsonParser(text).Document(out, error);
}

const Json& Manifest() {
  static const Json manifest = [] {
    std::ifstream in(std::string(WT_TEST_ASSETS_DIR) + "/manifest.json");
    std::stringstream ss;
    ss << in.rdbuf();
    Json json;
    std::string error;
    if (!ParseJson(ss.str(), &json, &error)) json = Json();
    return json;
  }();
  return manifest;
}

}  // namespace wt_test
