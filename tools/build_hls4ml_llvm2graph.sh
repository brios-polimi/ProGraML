#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact="${repo_root}/bazel-bin/programl/bin/llvm2graph-16"
install_path="${repo_root}/build/lib/programl/bin/llvm2graph-16"

bazel --batch build --workspace_status_command="true" \
  //programl/bin:llvm2graph-16

install -D -m 0755 "${artifact}" "${install_path}"
sha256sum "${install_path}"
