#!/bin/bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 -B "$project_root/scripts/release_pipeline.py" "$@"
