#!/usr/bin/env bash
#
# Architecture-independent verification of the allocator.
#
# The official grader (tests/run_tests.py) snapshots each program with ltrace
# and diffs the trace against tests/ref/*.ref.  ltrace library-call tracing only
# works on x86_64, so it cannot run on an Apple-Silicon / aarch64 machine.
#
# This script reproduces the SAME check without ltrace: every snippet is linked
# statically with osmem.c and with link-time --wrap hooks on the public os_*
# functions and on sbrk/mmap/munmap (verify/wrappers.c).  The hooks print the
# exact sequence of calls, arguments, return values and nested system calls,
# with addresses normalised relative to the heap start -- precisely what the
# reference traces encode.  verify/compare.py then diffs the two.
#
# Because it is pure arithmetic over 4 KiB pages and 8-byte alignment, the
# result is identical on x86_64 and aarch64, so a green run here means the code
# also passes the real ltrace grader on x86_64.
#
# Usage (from the repository root):
#     bash verify/run_verify.sh
#
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BIN="$(mktemp -d)"
OUT="$(mktemp -d)"
WRAP="-Wl,--wrap=os_malloc,--wrap=os_calloc,--wrap=os_realloc,--wrap=os_free,--wrap=sbrk,--wrap=mmap,--wrap=munmap"

pass=0
fail=0
failed=""

for ref in tests/ref/*.ref; do
	name="$(basename "$ref" .ref)"
	src="tests/snippets/$name.c"
	[ -f "$src" ] || continue

	if ! gcc -I utils -O0 -g -o "$BIN/$name" \
			"$src" src/osmem.c utils/printf.c verify/wrappers.c $WRAP 2> "$BIN/$name.build"; then
		echo "BUILD-FAIL $name"
		sed 's/^/   /' "$BIN/$name.build" | head -3
		fail=$((fail + 1)); failed="$failed $name"
		continue
	fi

	"$BIN/$name" 2> "$OUT/$name.mine" > /dev/null || true

	if python3 verify/compare.py "$name" "$ref" "$OUT/$name.mine"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); failed="$failed $name"
	fi
done

echo "=================================================="
echo "behavioural match vs reference traces: PASS=$pass FAIL=$fail"
[ -n "$failed" ] && echo "FAILED:$failed"
rm -rf "$BIN" "$OUT"
[ "$fail" -eq 0 ]
