#!/usr/bin/env bash
# tools/lint/check-threading-invariants.sh
#
# Enforces ADR-0007 invariants. Fails the build if any is violated. Run from
# the repo root; the CI lane invokes this after the build step.
#
# Invariants checked:
#   1. No std::shared_ptr<AssetHandle...> anywhere — asset handles must be
#      atomic-refcounted directly, not through shared_ptr.
#   2. No ImGui::GetDrawData() outside engine/imgui-integration/ — draw data
#      access must be confined to the integration layer that double-buffers it.
#   3. No raw MTLCommandBuffer / ID3D12GraphicsCommandList / VkCommandBuffer
#      references outside the corresponding rhi-* module — backend command
#      buffer types are externally synchronized and must not leak.
#
# Usage:
#   ./tools/lint/check-threading-invariants.sh           # fails on violation
#   ./tools/lint/check-threading-invariants.sh --verbose # show every match

set -euo pipefail

VERBOSE=0
if [[ "${1-}" == "--verbose" ]]; then VERBOSE=1; fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

violations=0

# Limit search to source we own. Comments and tests are excluded so the lint
# script itself doesn't trip on its own documentation strings or test fixtures.
search_dirs=(engine samples)
exclude_globs=(
    --exclude-dir=build
    --exclude-dir=.git
    --exclude-dir=vcpkg_installed
    --exclude-dir=tests
    --exclude=check-threading-invariants.sh
)

run_grep() {
    # grep returns 1 when no matches found — treat that as success.
    local pattern="$1"; shift
    local include_dirs=("$@")
    grep -rIn --include='*.h' --include='*.hpp' \
              --include='*.cpp' --include='*.cc' --include='*.mm' \
              "${exclude_globs[@]}" \
              -e "$pattern" "${include_dirs[@]}" 2>/dev/null || true
}

# 1. shared_ptr<AssetHandle...>
hits=$(run_grep 'std::shared_ptr<[[:space:]]*AssetHandle' "${search_dirs[@]}")
if [[ -n "$hits" ]]; then
    violations=$((violations+1))
    echo "ADR-0007 violation: std::shared_ptr<AssetHandle...> is forbidden"
    echo "$hits"
    echo
fi

# 2. ImGui::GetDrawData() outside engine/imgui-integration/
hits=$(run_grep 'ImGui::GetDrawData' "${search_dirs[@]}" | grep -v '^engine/imgui-integration/' || true)
if [[ -n "$hits" ]]; then
    violations=$((violations+1))
    echo "ADR-0007 violation: ImGui::GetDrawData() must stay in engine/imgui-integration/"
    echo "$hits"
    echo
fi

# 3. Raw command-buffer types outside their rhi-* module.
check_backend_type() {
    local type="$1" backend_dir="$2"
    local hits
    hits=$(run_grep "$type" "${search_dirs[@]}" | grep -v "^${backend_dir}/" || true)
    if [[ -n "$hits" ]]; then
        violations=$((violations+1))
        echo "ADR-0007 violation: $type leaked outside $backend_dir/"
        echo "$hits"
        echo
    elif [[ "$VERBOSE" == "1" ]]; then
        echo "ok: no $type references outside $backend_dir/"
    fi
}

check_backend_type 'MTLCommandBuffer'           'engine/rhi-metal'
check_backend_type 'ID3D12GraphicsCommandList'  'engine/rhi-d3d12'
check_backend_type 'VkCommandBuffer'            'engine/rhi-vulkan'

if [[ "$violations" -gt 0 ]]; then
    echo "check-threading-invariants: $violations violation(s) — see ADR-0007."
    exit 1
fi

echo "check-threading-invariants: ok"
exit 0
