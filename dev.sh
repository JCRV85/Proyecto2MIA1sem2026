#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd "$script_dir/.." && pwd)"
web_pid=""

cleanup() {
  if [[ -n "$web_pid" ]] && kill -0 "$web_pid" >/dev/null 2>&1; then
    kill "$web_pid" >/dev/null 2>&1 || true
    wait "$web_pid" >/dev/null 2>&1 || true
  fi
}

trap cleanup EXIT INT TERM

cd "$workspace"
echo "Iniciando frontend en segundo plano..."
npm run dev:web &
web_pid=$!

echo "Iniciando backend en primer plano..."
bash "$script_dir/run-api.sh"
