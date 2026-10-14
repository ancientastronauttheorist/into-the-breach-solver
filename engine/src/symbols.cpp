#include "itb/symbols.hpp"

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace itb {
namespace {

// Thread-safe: lookups share a reader lock, new names take the writer lock,
// and symbol_name reads a published pointer without locking (a name, once
// interned, never moves or changes).
struct Table {
  std::shared_mutex mu;
  std::deque<std::string> names{""};  // deque keeps the strings in place
  std::unordered_map<std::string_view, Symbol> ids;
  std::array<std::atomic<const std::string*>, 0x10000> by_id{};

  Table() { by_id[0].store(&names[0], std::memory_order_release); }
};

Table& table() {
  static Table t;
  return t;
}

}  // namespace

namespace {

Symbol intern_shared(std::string_view name) {
  Table& t = table();
  {
    std::shared_lock lock(t.mu);
    if (auto it = t.ids.find(name); it != t.ids.end()) return it->second;
  }
  std::unique_lock lock(t.mu);
  if (auto it = t.ids.find(name); it != t.ids.end()) return it->second;
  if (t.names.size() > 0xFFFF) throw std::length_error("symbol table full");
  const auto id = static_cast<Symbol>(t.names.size());
  t.names.emplace_back(name);
  t.ids.emplace(t.names.back(), id);
  t.by_id[id].store(&t.names.back(), std::memory_order_release);
  return id;
}

}  // namespace

Symbol intern(std::string_view name) {
  if (name.empty()) return kNoSymbol;
  // Each thread keeps the names it has seen (keys point at the table's own
  // copies, which never move): with several search threads, every lookup
  // through the shared lock contended on it.
  thread_local std::unordered_map<std::string_view, Symbol> seen;
  if (auto it = seen.find(name); it != seen.end()) return it->second;
  const Symbol id = intern_shared(name);
  seen.emplace(symbol_name(id), id);
  return id;
}

Symbol find_symbol(std::string_view name) {
  Table& t = table();
  std::shared_lock lock(t.mu);
  auto it = t.ids.find(name);
  return it == t.ids.end() ? kNoSymbol : it->second;
}

std::string_view symbol_name(Symbol s) {
  const std::string* name = table().by_id[s].load(std::memory_order_acquire);
  return name ? std::string_view(*name) : std::string_view();
}

}  // namespace itb
