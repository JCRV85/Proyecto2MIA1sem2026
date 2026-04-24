#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd "$script_dir/.." && pwd)"
source_root="$workspace/apps/api-cpp"
build_root="$workspace/.build/api"
output="$build_root/mia-api"

mkdir -p "$build_root"

compiler="${CXX:-}"

if [[ -n "$compiler" ]]; then
  if ! command -v "$compiler" >/dev/null 2>&1; then
    echo "El compilador indicado en CXX no existe en PATH: $compiler" >&2
    exit 1
  fi
else
  for candidate in g++ clang++; do
    if command -v "$candidate" >/dev/null 2>&1; then
      compiler="$candidate"
      break
    fi
  done
fi

if [[ -z "$compiler" ]]; then
  echo "No se encontro un compilador C++ compatible en PATH." >&2
  echo "Instala g++ o clang++ y vuelve a ejecutar este script." >&2
  exit 1
fi

sources=(
  "$source_root/src/main.cpp"
  "$source_root/src/engine_state.cpp"
  "$source_root/src/engine_disk.cpp"
  "$source_root/src/engine_storage.cpp"
  "$source_root/src/engine_auth.cpp"
  "$source_root/src/engine_fs.cpp"
  "$source_root/src/engine_reports.cpp"
  "$source_root/src/engine_utils.cpp"
)

args=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pthread
  -I "$source_root/include"
  -I "$source_root/vendor"
  "${sources[@]}"
  -o "$output"
)

echo "Compilando backend con $compiler..."
"$compiler" "${args[@]}"
echo "Backend compilado en $output"
