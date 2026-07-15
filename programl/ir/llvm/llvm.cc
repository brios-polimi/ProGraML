// Contact Chris Cummins <chrisc.101@gmail.com>.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "programl/ir/llvm/llvm.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "labm8/cpp/status.h"
#include "labm8/cpp/status_macros.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#if LLVM_VERSION_MAJOR < 16
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#endif
#include "programl/ir/llvm/internal/program_graph_builder_pass.h"
#include "programl/proto/program_graph.pb.h"

using labm8::Status;

namespace programl {
namespace ir {
namespace llvm {

namespace {

bool ParseMetadataId(const ::llvm::StringRef line, size_t start, uint64_t* id,
                     size_t* end) {
  if (start >= line.size() || line[start] != '!' || start + 1 >= line.size() ||
      !std::isdigit(static_cast<unsigned char>(line[start + 1]))) {
    return false;
  }

  uint64_t value = 0;
  size_t cursor = start + 1;
  while (cursor < line.size() && std::isdigit(static_cast<unsigned char>(line[cursor]))) {
    value = value * 10 + line[cursor] - '0';
    ++cursor;
  }
  *id = value;
  *end = cursor;
  return true;
}

bool ReferencesDebugMetadata(const ::llvm::StringRef line,
                             const std::unordered_set<uint64_t>& debugMetadata) {
  for (size_t cursor = 0; cursor < line.size();) {
    const size_t reference = line.find('!', cursor);
    if (reference == ::llvm::StringRef::npos) {
      return false;
    }
    uint64_t id;
    size_t end;
    if (ParseMetadataId(line, reference, &id, &end) && debugMetadata.count(id)) {
      return true;
    }
    cursor = reference + 1;
  }
  return false;
}

// Program graphs do not use source-level debug information. LLVM's textual IR
// parser, however, eagerly parses the complete DWARF metadata graph. In IR
// emitted with -g this can be considerably larger than the program itself.
// Remove the debug-only portions before handing the buffer to LLVM, while
// retaining all non-debug metadata (e.g. branch weights).
std::string StripDebugInfo(const ::llvm::StringRef ir) {
  // First find every numbered metadata node reachable from a !DI* node. LLVM
  // also emits tuple nodes containing only debug references, so checking only
  // for !DI* definitions leaves dangling metadata IDs behind.
  std::unordered_set<uint64_t> debugMetadata;
  bool changed = true;
  while (changed) {
    changed = false;
    size_t lineStart = 0;
    while (lineStart < ir.size()) {
      const size_t lineEnd = ir.find('\n', lineStart);
      const size_t length = lineEnd == ::llvm::StringRef::npos ? ir.size() - lineStart
                                                                : lineEnd - lineStart;
      const ::llvm::StringRef line = ir.substr(lineStart, length);
      uint64_t id;
      size_t idEnd;
      if (ParseMetadataId(line, 0, &id, &idEnd) &&
          line.substr(idEnd).startswith(" = ") &&
          (line.contains("= !DI") || line.contains("= distinct !DI") ||
           ReferencesDebugMetadata(line.substr(idEnd + 3), debugMetadata))) {
        changed |= debugMetadata.insert(id).second;
      }
      if (lineEnd == ::llvm::StringRef::npos) {
        break;
      }
      lineStart = lineEnd + 1;
    }
  }

  std::string stripped;
  stripped.reserve(ir.size());

  size_t lineStart = 0;
  while (lineStart < ir.size()) {
    const size_t lineEnd = ir.find('\n', lineStart);
    const size_t length = lineEnd == ::llvm::StringRef::npos ? ir.size() - lineStart
                                                              : lineEnd - lineStart;
    const ::llvm::StringRef line = ir.substr(lineStart, length);

    // Numbered !DI* nodes and !llvm.dbg.* named metadata are only reachable
    // through !dbg attachments or llvm.dbg.* intrinsics, which are removed
    // below. Do not remove other metadata such as !prof.
    uint64_t metadataId;
    size_t metadataIdEnd;
    const bool isDebugMetadata = line.startswith("!llvm.dbg.") ||
                                 (ParseMetadataId(line, 0, &metadataId, &metadataIdEnd) &&
                                  debugMetadata.count(metadataId));
    const bool isDebugIntrinsic =
        !line.startswith("declare") && line.contains("@llvm.dbg.");
    if (!isDebugMetadata && !isDebugIntrinsic) {
      std::string withoutDebugLocations;
      size_t cursor = 0;
      while (true) {
        const size_t attachment = line.find("!dbg !", cursor);
        if (attachment == ::llvm::StringRef::npos) {
          withoutDebugLocations.append(line.data() + cursor, line.size() - cursor);
          break;
        }
        // Instruction/global attachments use ", !dbg !N", whereas function
        // definitions use " !dbg !N" immediately before their body.
        size_t attachmentStart = attachment;
        if (attachmentStart > cursor && line[attachmentStart - 1] == ' ') {
          --attachmentStart;
          if (attachmentStart > cursor && line[attachmentStart - 1] == ',') {
            --attachmentStart;
          }
        }
        withoutDebugLocations.append(line.data() + cursor, attachmentStart - cursor);
        cursor = attachment + 6;  // Skip "!dbg !".
        while (cursor < line.size() && std::isdigit(static_cast<unsigned char>(line[cursor]))) {
          ++cursor;
        }
      }

      // A non-debug attachment (for example !llvm.loop) can reference a
      // debug-only metadata tuple. Drop that attachment too, otherwise LLVM
      // rejects the now-removed tuple as an undefined metadata reference.
      const ::llvm::StringRef withoutDbg(withoutDebugLocations);
      cursor = 0;
      while (true) {
        const size_t attachment = withoutDbg.find(", !", cursor);
        if (attachment == ::llvm::StringRef::npos) {
          stripped.append(withoutDbg.data() + cursor, withoutDbg.size() - cursor);
          break;
        }
        const size_t tagEnd = withoutDbg.find(' ', attachment + 2);
        uint64_t id;
        size_t idEnd;
        if (tagEnd != ::llvm::StringRef::npos &&
            ParseMetadataId(withoutDbg, tagEnd + 1, &id, &idEnd) &&
            debugMetadata.count(id)) {
          stripped.append(withoutDbg.data() + cursor, attachment - cursor);
          cursor = idEnd;
        } else {
          // This is a non-debug attachment; retain it and continue looking.
          stripped.append(withoutDbg.data() + cursor, attachment + 2 - cursor);
          cursor = attachment + 2;
        }
      }
      stripped.push_back('\n');
    }

    if (lineEnd == ::llvm::StringRef::npos) {
      break;
    }
    lineStart = lineEnd + 1;
  }

  return stripped;
}

}  // namespace

Status BuildProgramGraph(::llvm::Module& module, ProgramGraph* graph,
                         const ProgramGraphOptions& options) {
  ::llvm::legacy::PassManager passManager;

#if LLVM_VERSION_MAJOR < 16
  // PassManagerBuilder removed in LLVM 16. For graph construction against
  // HLS IR we do not optimize — the input IR is already processed by the
  // HLS tool's own pipeline.
  ::llvm::PassManagerBuilder passManagerBuilder;
  passManagerBuilder.OptLevel = options.opt_level();
  passManagerBuilder.populateModulePassManager(passManager);
#endif

  internal::ProgramGraphBuilderPass* pass = new internal::ProgramGraphBuilderPass(options);
  passManager.add(pass);
  passManager.run(module);
  ASSIGN_OR_RETURN(*graph, pass->GetProgramGraph());
  return Status::OK;
}

Status BuildProgramGraph(const ::llvm::MemoryBuffer& irBuffer, ProgramGraph* graph,
                         const ProgramGraphOptions& options) {
  ::llvm::SMDiagnostic error;
  ::llvm::LLVMContext ctx;
#if LLVM_VERSION_MAJOR >= 16
  ctx.setOpaquePointers(false);  // Keep typed pointers internally
#endif
  const std::string strippedIr = StripDebugInfo(irBuffer.getBuffer());
  auto module = ::llvm::parseIR(::llvm::MemoryBufferRef(strippedIr, irBuffer.getBufferIdentifier()),
                                error, ctx);
  if (!module) {
    // Format an error message in the style of clang, complete with line number,
    // column number, then the offending line and a caret pointing at the
    // column. For example:
    //
    // 1:0: error: expected top-level entity
    //     node {
    //     ^
    return Status(labm8::error::Code::INVALID_ARGUMENT, "{}:{}: error: {}\n    {}\n    {}^",
                  error.getLineNo(), error.getColumnNo(), error.getMessage().str(),
                  error.getLineContents().str(), string(std::max(error.getColumnNo() - 1, 0), ' '));
  }

  return BuildProgramGraph(*module, graph, options);
}

Status BuildProgramGraph(const string& irString, ProgramGraph* graph,
                         const ProgramGraphOptions& options) {
  const auto irBuffer = ::llvm::MemoryBuffer::getMemBuffer(irString);
  return BuildProgramGraph(*irBuffer, graph, options);
}

}  // namespace llvm
}  // namespace ir
}  // namespace programl
