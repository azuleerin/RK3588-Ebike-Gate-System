#!/usr/bin/env bash
# 根目录的薄包装: 让客户在工程根目录直接敲 ./run_acceptance_demo.sh
# 真正的逻辑在 scripts/run_acceptance_demo.sh
exec "$(cd "$(dirname "$0")" && pwd)/scripts/run_acceptance_demo.sh" "$@"
