#!/usr/bin/env bash
# Installs the reference toolchain into tools/ (git-ignored):
#   - OSS CAD Suite: Yosys + slang plugin, SymbiYosys, ABC, rIC3, Pono, Verilator, ...
#   - EBMC 6.0, built from source (no macOS binary is published)
#   - FVEval benchmark (NVIDIA, Apache-2.0)
# Usage: scripts/setup_tools.sh [oss-cad-suite release tag, default: latest]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tools="$root/tools"
mkdir -p "$tools"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) plat=darwin-arm64 ;;
    Darwin-x86_64) plat=darwin-x64 ;;
    Linux-x86_64) plat=linux-x64 ;;
    Linux-aarch64) plat=linux-arm64 ;;
    *) echo "unsupported platform" >&2; exit 1 ;;
esac

if [ ! -x "$tools/oss-cad-suite/bin/yosys" ]; then
    tag=${1:-$(curl -s https://api.github.com/repos/YosysHQ/oss-cad-suite-build/releases/latest \
               | python3 -c "import sys,json; print(json.load(sys.stdin)['tag_name'])")}
    url="https://github.com/YosysHQ/oss-cad-suite-build/releases/download/$tag/oss-cad-suite-$plat-${tag//-/}.tgz"
    echo "downloading $url"
    curl -sL "$url" | tar -xz -C "$tools"
fi

if [ ! -x "$tools/hw-cbmc/src/ebmc/ebmc" ]; then
    [ -d "$tools/hw-cbmc" ] || git clone -q --depth 1 --branch ebmc-6.0 https://github.com/diffblue/hw-cbmc.git "$tools/hw-cbmc"
    cd "$tools/hw-cbmc"
    git submodule update -q --init --depth 1
    if [ "$(uname -s)" = Darwin ]; then
        # Needs GNU bison/flex; and the pinned macOS 10.15 target is rejected by recent libc++.
        brew list bison >/dev/null 2>&1 || brew install bison flex
        export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
        sed -i '' 's/-mmacosx-version-min=10.15/-mmacosx-version-min=13.0/g' lib/cbmc/src/common
    fi
    make -C lib/cbmc/src minisat2-download
    make -C src -j"$(getconf _NPROCESSORS_ONLN)"
fi

[ -d "$tools/FVEval" ] || git clone -q --depth 1 https://github.com/NVlabs/FVEval.git "$tools/FVEval"

# Certificate checkers (M5): lidrup-check for bounded claims, Certifaiger for proofs.
if [ ! -x "$tools/lidrup-check/lidrup-check" ]; then
    [ -d "$tools/lidrup-check" ] || git clone -q --depth 1 https://github.com/arminbiere/lidrup-check.git "$tools/lidrup-check"
    (cd "$tools/lidrup-check" && ./configure > /dev/null && make lidrup-check > /dev/null)
fi
if [ ! -x "$tools/certifaiger/build/certifaiger" ]; then
    [ -d "$tools/certifaiger" ] || git clone -q --depth 1 https://github.com/Froleyks/certifaiger.git "$tools/certifaiger"
    cd "$tools/certifaiger"
    git submodule update -q --init --depth 1
    static=ON
    if [ "$(uname -s)" = Darwin ]; then
        # clang rejects some GCC-only warnings, macOS cannot link statically, and
        # AIGER's extension-less 'version'/'format' files shadow C++ headers on a
        # case-insensitive filesystem.
        sed -i '' -e 's/-Wduplicated-cond//g; s/-Wduplicated-branches//g; s/-Wlogical-op//g; s/-Wcast-align=strict//g' util.cmake CMakeLists.txt
        static=OFF
        cmake -DCMAKE_BUILD_TYPE=Release -B build -DSTATIC=$static -DCHECK=ON > /dev/null
        cmake --build build --target aiger > /dev/null 2>&1 || true
        for f in version format; do [ -f build/_deps/aiger-src/$f ] && mv build/_deps/aiger-src/$f build/_deps/aiger-src/$f.txt; done
    fi
    cmake -DCMAKE_BUILD_TYPE=Release -B build -DSTATIC=$static -DCHECK=ON > /dev/null
    cmake --build build -j --target certifaiger kissat > /dev/null
    cd "$root"
fi

echo "ok: $("$tools/oss-cad-suite/bin/yosys" -V | cut -d' ' -f1-2), rIC3 $("$tools/oss-cad-suite/bin/rIC3" --version | cut -d' ' -f2), EBMC $("$tools/hw-cbmc/src/ebmc/ebmc" --version), lidrup-check, certifaiger"
echo "PATH for qfv: export PATH=$tools/oss-cad-suite/bin:$tools/lidrup-check:$tools/certifaiger/build:\$PATH"
