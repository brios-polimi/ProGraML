#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace programl {
namespace ir {
namespace llvm {
namespace internal {

// Compact source-identity information captured from textual IR before the
// expensive DWARF metadata graph is removed.
struct DebugInfoIndex {
  // LLVM function name -> source DISubprogram metadata ID.
  std::unordered_map<std::string, int64_t> function_debug_ids;

  // LLVM function name -> debug-location ID for each non-debug instruction,
  // in module/basic-block/instruction order. A value of -1 means no !dbg.
  std::unordered_map<std::string, std::vector<int64_t>> instruction_debug_ids;

  // LLVM function name -> LLVM SSA value (e.g. "%5") -> DILocalVariable ID.
  std::unordered_map<std::string, std::unordered_map<std::string, int64_t>>
      value_variable_ids;
};

}  // namespace internal
}  // namespace llvm
}  // namespace ir
}  // namespace programl
