#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd "$script_dir/.." && pwd)"
binary="$workspace/.build/api/mia-api"

if [[ ! -x "$binary" ]]; then
  echo "No existe el binario del backend. Intentando compilar..."
  bash "$script_dir/build-api.sh"
fi

exec "$binary"
