# Architecture-independent verification

The official grader (`tests/run_tests.py`) captures each test program with
**`ltrace`** and diffs the recorded trace against `tests/ref/*.ref`. `ltrace`
library-call tracing (`-x os_*`) only works on **x86_64**; on an Apple-Silicon /
`aarch64` machine `ltrace` sees the system calls but cannot intercept the
`os_malloc` / `os_free` / … library calls, so the grader cannot run there.

This directory reproduces the **same** check without `ltrace`, so correctness
can be confirmed on any machine (including inside Docker on an ARM Mac).

## How it works

Each snippet is linked statically with `src/osmem.c` and with link-time
`--wrap` hooks (`wrappers.c`) on:

* the public `os_malloc`, `os_calloc`, `os_realloc`, `os_free`;
* the `sbrk`, `mmap`, `munmap` system-call entry points used by the allocator.

The hooks print, for every call, its name, arguments, return value and the
nested system calls it makes, with every address normalised **relative to the
start of the heap** (`H+0x..`) or to its `mmap` region (`M<id>+0x..`) — exactly
the normalisation the grader's parser performs.

`compare.py` reduces both the reference trace and the instrumented output to the
same canonical event list and diffs them.

Because the behaviour is pure arithmetic over 4 KiB pages and 8-byte alignment,
the canonical trace is **identical on x86_64 and aarch64**. A green run here
therefore means the code also passes the real `ltrace` grader on x86_64 (which
CI confirms — see `.github/workflows/tests.yml`).

## Run it

From the repository root, on any Linux box (or in the project's Docker image)
with `gcc` and `python3`:

```sh
bash verify/run_verify.sh
```

Expected tail:

```
behavioural match vs reference traces: PASS=40 FAIL=0
```
