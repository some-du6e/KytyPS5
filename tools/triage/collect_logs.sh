#!/bin/sh
# Zips the newest KytyPS5 session log, _kyty.txt and a `vulkaninfo --summary` dump for a bug report.
#
#   sh collect_logs.sh [KYTY_DIR] [OUT_DIR]
#
# KYTY_DIR is the folder with the emulator (default: the current folder). Reads
# KYTY_DIR/_Logs/*.log (newest) and KYTY_DIR/_kyty.txt, writes OUT_DIR/kyty-logs-<date>.zip.
# Nothing is uploaded. A printf log over MAX_PRINTF_MB (default 256) is cut to its first 32 MiB
# and last 128 MiB; the crash report is at the end.
#
# The logs contain your user name and the game folder path. To paste a redacted summary instead:
#   python3 -I tools/triage/kyty_triage.py --redact kyty-logs-<date>.zip
set -eu

kyty_dir=${1:-.}
out_dir=${2:-.}
max_mb=${MAX_PRINTF_MB:-256}
stamp=$(date +%Y%m%d-%H%M%S)
out_dir=$(cd "$out_dir" && pwd)
zip_path="$out_dir/kyty-logs-$stamp.zip"
n=1
while [ -e "$zip_path" ]; do
	zip_path="$out_dir/kyty-logs-$stamp-$n.zip"
	n=$((n + 1))
done
stage=$(mktemp -d "${TMPDIR:-/tmp}/kyty-logs.XXXXXX")

cleanup() {
	# Only the mktemp folder created above is removed.
	case "$stage" in
		"${TMPDIR:-/tmp}"/kyty-logs.*) rm -rf -- "$stage" ;;
	esac
}
trap cleanup EXIT INT TERM

info="$stage/collect_info.txt"
{
	echo "collected: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "kyty dir: $(cd "$kyty_dir" && pwd)"
	echo "os: $(uname -srmo 2>/dev/null || uname -a)"
} >"$info"

session=""
if [ -d "$kyty_dir/_Logs" ]; then
	# newest *.log by modification time
	session=$(ls -t "$kyty_dir/_Logs"/*.log 2>/dev/null | head -n 1 || true)
fi
if [ -n "$session" ] && [ -f "$session" ]; then
	cp -- "$session" "$stage/"
	echo "session log: $(basename "$session") ($(wc -c <"$session" | tr -d ' ') bytes)" >>"$info"
else
	echo "session log: none in $kyty_dir/_Logs (pass --session-log <file> to the emulator)" >>"$info"
fi

printf_log="$kyty_dir/_kyty.txt"
if [ -f "$printf_log" ]; then
	size=$(wc -c <"$printf_log" | tr -d ' ')
	if [ "$size" -le $((max_mb * 1048576)) ]; then
		cp -- "$printf_log" "$stage/_kyty.txt"
		echo "printf log: _kyty.txt ($size bytes)" >>"$info"
	else
		head -c $((32 * 1048576)) "$printf_log" >"$stage/_kyty.head.txt"
		tail -c $((128 * 1048576)) "$printf_log" >"$stage/_kyty.tail.txt"
		echo "printf log: _kyty.txt is $size bytes; kept the first 32 MiB and the last 128 MiB" >>"$info"
	fi
else
	echo "printf log: no _kyty.txt (it is off by default: --printf-direction File)" >>"$info"
fi

if command -v vulkaninfo >/dev/null 2>&1; then
	vulkaninfo --summary >"$stage/vulkaninfo.txt" 2>&1 || true
	echo "vulkaninfo: $(command -v vulkaninfo)" >>"$info"
else
	echo "vulkaninfo: not found (package vulkan-tools, or the Vulkan SDK on macOS)" >>"$info"
fi

if command -v zip >/dev/null 2>&1; then
	(cd "$stage" && zip -q -9 -r "$zip_path" .)
elif command -v python3 >/dev/null 2>&1; then
	(cd "$stage" && python3 -I -m zipfile -c "$zip_path" $(ls -A))
else
	echo "need zip or python3 to write the archive" >&2
	exit 1
fi
echo "Wrote $zip_path"
echo "It contains your user name and game folder path. Attach it, or paste the output of:"
echo "  python3 -I tools/triage/kyty_triage.py --redact \"$zip_path\""
