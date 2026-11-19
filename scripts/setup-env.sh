#!/usr/bin/env bash
# 開発環境セットアップスクリプト (Ubuntu / Debian 系, WSL2 対応)
#
# 実行内容:
#   1. ビルド・開発に必要なパッケージの導入 (g++, cmake, ninja, clangd など)
#   2. CMake の構成 (compile_commands.json を生成 → clangd が参照)
#   3. ビルドとテスト実行 (任意)
#
# 使い方:
#   ./scripts/setup-env.sh              # 一通り実行 (パッケージ導入 + 構成 + ビルド + テスト)
#   ./scripts/setup-env.sh --no-build   # パッケージ導入と CMake 構成のみ
#   ./scripts/setup-env.sh --skip-apt   # パッケージ導入を飛ばす
#   ./scripts/setup-env.sh --no-test    # ビルドまで行い、テストは実行しない
#   ./scripts/setup-env.sh --type Release
#   ./scripts/setup-env.sh --submodules # private-repo も取得 (要アクセス権)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="Debug"
DO_APT=1
DO_BUILD=1
DO_TEST=1
DO_SUBMODULES=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-apt)   DO_APT=0 ;;
        --no-build)   DO_BUILD=0; DO_TEST=0 ;;
        --no-test)    DO_TEST=0 ;;
        --submodules) DO_SUBMODULES=1 ;;
        --type)       shift; BUILD_TYPE="${1:?--type にはビルドタイプを指定してください}" ;;
        -h|--help)    sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "不明なオプション: $1" >&2; exit 1 ;;
    esac
    shift
done

log() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- packages
install_packages() {
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "apt-get が見つかりません。Ubuntu/Debian 以外では --skip-apt を使い、" >&2
        echo "g++ cmake ninja clangd clang-tidy clang-format を手動で導入してください。" >&2
        exit 1
    fi

    local SUDO=""
    if [[ $EUID -ne 0 ]]; then
        command -v sudo >/dev/null 2>&1 || { echo "root 権限または sudo が必要です" >&2; exit 1; }
        SUDO="sudo"
    fi

    log "パッケージを導入します"
    $SUDO apt-get update
    $SUDO apt-get install -y --no-install-recommends \
        build-essential g++ gdb git ca-certificates curl unzip \
        cmake ninja-build pkg-config \
        clang clangd clang-tidy clang-format \
        lcov python3
    # ASan (-fsanitize=address) は libasan が必要 (build-essential の g++ に同梱)
}

# ---------------------------------------------------------------- checks
check_tools() {
    log "ツールのバージョン確認"
    local missing=0
    for tool in g++ cmake ninja clangd clang-tidy clang-format; do
        if command -v "$tool" >/dev/null 2>&1; then
            printf '  %-13s %s\n' "$tool" "$("$tool" --version 2>&1 | head -n1)"
        else
            printf '  %-13s \033[1;31m未インストール\033[0m\n' "$tool"
            missing=1
        fi
    done
    [[ $missing -eq 0 ]] || { echo "不足しているツールがあります" >&2; exit 1; }

    # README の要件: CMake 3.20+
    local ver
    ver="$(cmake --version | head -n1 | awk '{print $3}')"
    if [[ "$(printf '%s\n3.20\n' "$ver" | sort -V | head -n1)" != "3.20" ]]; then
        echo "CMake 3.20 以上が必要です (現在: $ver)" >&2
        exit 1
    fi
}

# ---------------------------------------------------------------- project
configure() {
    cd "$ROOT_DIR"

    if [[ $DO_SUBMODULES -eq 1 ]]; then
        log "サブモジュールを取得します"
        git submodule update --init --recursive
    fi

    # .clangd は CompilationDatabase: build を指定しているため、
    # build/compile_commands.json が必要。
    log "CMake を構成します (${BUILD_TYPE}, build/compile_commands.json を生成)"
    cmake -S . -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

    [[ -f "$BUILD_DIR/compile_commands.json" ]] \
        || { echo "compile_commands.json が生成されませんでした" >&2; exit 1; }
}

build_and_test() {
    log "ビルドします"
    cmake --build "$BUILD_DIR"

    if [[ $DO_TEST -eq 1 ]]; then
        log "テストを実行します"
        ctest --test-dir "$BUILD_DIR" --output-on-failure
    fi
}

# ---------------------------------------------------------------- main
[[ $DO_APT -eq 1 ]] && install_packages
check_tools
configure
[[ $DO_BUILD -eq 1 ]] && build_and_test

cat <<EOF

セットアップ完了。

clangd (VS Code):
  1. 拡張機能 "clangd" (llvm-vs-code-extensions.vscode-clangd) を導入
  2. 競合を避けるため "C/C++" (ms-vscode.cpptools) の IntelliSense は無効化
  3. 設定例 (.vscode/settings.json):
       "C_Cpp.intelliSenseEngine": "disabled",
       "clangd.arguments": ["--background-index", "--clang-tidy", "--header-insertion=never"]
  4. CMake 構成を変えたら "clangd: Restart language server" を実行
EOF
