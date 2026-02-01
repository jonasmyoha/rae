#!/usr/bin/env bash
# Collect compiler source metrics (file + line counts) and append to stats/compiler_metrics.jsonl

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC_DIR="$ROOT_DIR/compiler/src"
STATS_DIR="$ROOT_DIR/stats"
METRICS_FILE="$STATS_DIR/compiler_metrics.jsonl"
LATEST_FILE="$STATS_DIR/compiler_metrics_latest.txt"

if [ ! -d "$SRC_DIR" ]; then
  echo "error: compiler/src directory not found at $SRC_DIR" >&2
  exit 1
fi

FILE_COUNT=0
LINE_COUNT=0
while IFS= read -r -d '' file; do
  if [ -f "$file" ]; then
    FILE_COUNT=$((FILE_COUNT + 1))
    lines=$(wc -l < "$file")
    LINE_COUNT=$((LINE_COUNT + lines))
  fi
done < <(find "$SRC_DIR" -type f \( -name '*.c' -o -name '*.h' \) -print0 | sort -z)

if [ "$FILE_COUNT" -eq 0 ]; then
  echo "error: no .c/.h files found under $SRC_DIR" >&2
  exit 1
fi

CURRENT_COMMIT="$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo "none")"

DECISION="$(NEW_FILE_COUNT=$FILE_COUNT NEW_LINE_COUNT=$LINE_COUNT CURRENT_COMMIT="$CURRENT_COMMIT" METRICS_FILE="$METRICS_FILE" python3 - <<'PY'
import json, os, sys, datetime
path = os.environ["METRICS_FILE"]
new_files = int(os.environ["NEW_FILE_COUNT"])
new_lines = int(os.environ["NEW_LINE_COUNT"])
new_commit = os.environ["CURRENT_COMMIT"]
now = datetime.datetime.now()
today_str = now.strftime("%Y-%m-%d")

try:
    with open(path, "r", encoding="utf-8") as fh:
        lines = [line.strip() for line in fh if line.strip()]
except FileNotFoundError:
    print("append")
    sys.exit(0)

if not lines:
    print("append")
    sys.exit(0)

try:
    data = json.loads(lines[-1])
except json.JSONDecodeError:
    print("append")
    sys.exit(0)

last_timestamp = data.get("timestamp", "")
is_today = last_timestamp.startswith(today_str)

if data.get("src_file_count") == new_files and data.get("src_line_count") == new_lines:
    if data.get("commit", "") == new_commit:
        print("skip")
        print(f"Compiler metrics unchanged for commit {new_commit}; skipping update.")
        sys.exit(0)

if is_today:
    print("replace")
else:
    print("append")
PY
)"

IFS=$'\n' read -r DECISION_CODE DECISION_MSG <<<"$DECISION"
if [ "$DECISION_CODE" = "skip" ]; then
  if [ -n "$DECISION_MSG" ]; then
    echo "$DECISION_MSG"
  else
    echo "Compiler metrics unchanged; skipping update."
  fi
  exit 0
fi

TIMESTAMP="$(date +"%Y-%m-%d %H:%M:%S %z")"
mkdir -p "$STATS_DIR"
JSON_ENTRY=$(printf '{"timestamp":"%s","commit":"%s","src_file_count":%d,"src_line_count":%d}' "$TIMESTAMP" "$CURRENT_COMMIT" "$FILE_COUNT" "$LINE_COUNT")

if [ "$DECISION_CODE" = "replace" ]; then
  # Remove the last line and append the new one
  if [[ "$OSTYPE" == "darwin"* ]]; then
    sed -i '' '$d' "$METRICS_FILE"
  else
    sed -i '$d' "$METRICS_FILE"
  fi
  echo "$JSON_ENTRY" >> "$METRICS_FILE"
  echo "Updated today's compiler metrics: $JSON_ENTRY"
else
  echo "$JSON_ENTRY" >> "$METRICS_FILE"
  echo "Recorded compiler metrics: $JSON_ENTRY"
fi

cat > "$LATEST_FILE" <<NOTE
Rae compiler metrics
Timestamp: $TIMESTAMP
Commit: $CURRENT_COMMIT
Source directory: $SRC_DIR
Source files (.c/.h): $FILE_COUNT
Total lines: $LINE_COUNT
NOTE

echo "Recorded compiler metrics: $JSON_ENTRY"
