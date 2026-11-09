// test_assets/manifest.json for the native tests: a minimal JSON reader (the
// tests take no JSON dependency) plus helpers for the fields they compare.
#ifndef WT_NATIVE_TEST_MANIFEST_H_
#define WT_NATIVE_TEST_MANIFEST_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wt_test {

struct Json {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Json> array;
  std::vector<std::pair<std::string, Json>> object;

  // The member named key, or a null value when this isn't an object or has
  // no such member.
  const Json& operator[](const std::string& key) const;
  bool has(const std::string& key) const;
  // The number as an integer (rounded), or 0 for a non-number.
  int64_t Int() const;
  // An array of numbers as integers; empty for anything else.
  std::vector<int64_t> Ints() const;
};

// Parses a complete JSON document. False, with *error set, on bad input.
bool ParseJson(const std::string& text, Json* out, std::string* error);

// test_assets/manifest.json, parsed once. Aborts the test binary (via a
// failed gtest assertion in the caller) when unreadable: callers check
// Manifest()["files"].array is non-empty.
const Json& Manifest();

}  // namespace wt_test

#endif  // WT_NATIVE_TEST_MANIFEST_H_
