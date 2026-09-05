#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
case "$(uname -s)" in
  Darwin) preset=macos ;;
  Linux) preset=linux ;;
  *) echo 'Use scripts/build.ps1 on Windows.' >&2; exit 1 ;;
esac
cmake --preset "$preset"
cmake --build --preset "$preset" --parallel
if [[ "${1:-}" == '--run' ]]; then
  if [[ "$preset" == macos ]]; then
    open "$project_root/build/macos/bin/CodexSwitcher.app"
  else
    exec "$project_root/build/linux/bin/codex-switcher"
  fi
fi
