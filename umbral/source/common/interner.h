#pragma once

#include <cstring>
#include <memory_resource>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <common/types.h>

using SymId = u32;

struct SvHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const noexcept {
    size_t h = 14695981039346656037u;
    for (unsigned char c : s) {
      h ^= c;
      h *= 1099511628211u;
    }
    return h;
  }
};

struct SvEq {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};

class Interner {
public:
  explicit Interner(
      std::pmr::memory_resource *mr = std::pmr::get_default_resource())
      : mr(mr), pool(mr), strings(mr), map(0, SvHash{}, SvEq{}, mr) {}

  SymId intern(std::string_view s) {
    // check if we already had it interned
    if (auto it = map.find(s); it != map.end())
      return it->second;

    // copy bytes into owned memory
    std::string_view stored = store(s);

    SymId id = static_cast<SymId>(strings.size());
    strings.push_back(stored);
    map.emplace(stored, id);
    return id;
  }

  std::string_view view(SymId id) const { return strings.at(id); }

  size_t size() const { return strings.size(); }

private:
  std::pmr::memory_resource *mr;
  std::pmr::monotonic_buffer_resource pool;
  std::pmr::vector<std::string_view> strings;
  std::pmr::unordered_map<std::string_view, SymId, SvHash, SvEq> map;

  std::string_view store(std::string_view s) {
    char *mem = static_cast<char *>(pool.allocate(s.size() + 1, alignof(char)));
    std::memcpy(mem, s.data(), s.size());
    mem[s.size()] = '\0';
    return std::string_view(mem, s.size());
  }
};
