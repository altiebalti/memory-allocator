#!/usr/bin/env python3
"""Compare the allocator's instrumented behaviour against a reference ltrace trace.

Both sides are reduced to the same canonical list of events:
  - public calls:  "NAME(arg,arg) = RET"
  - nested brk:    "brk ARG = RET"
  - nested mmap:   "mmap len=N = RET"
  - nested munmap: "munmap PTR len=N"
Addresses are normalised to H+0x.. (heap, relative to heap start) or M<id>+0x..
(mmap region), exactly as the grader's parser does.
"""
import ast
import re
import sys
import difflib


def norm_token(s):
    s = s.strip()
    if s in ("<void>", "void"):
        return "void"
    m = re.match(r"<mapped-addr(\d+)>(?:\s*\+\s*(0x[0-9a-fA-F]+))?$", s)
    if m:
        return "M%s+%s" % (m.group(1), (m.group(2) or "0x0").lower())
    m = re.match(r"M(\d+)\+(0x[0-9a-fA-F]+)$", s)
    if m:
        return "M%s+%s" % (m.group(1), m.group(2).lower())
    m = re.match(r"HeapStart\s*\+\s*(0x[0-9a-fA-F]+)$", s)
    if m:
        return "H+" + m.group(1).lower()
    m = re.match(r"H\+(0x[0-9a-fA-F]+)$", s)
    if m:
        return "H+" + m.group(1).lower()
    return s


def parse_ref(path):
    events = []
    with open(path) as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            if line.startswith("+++") or line.startswith("DBG"):
                continue
            indented = line[0] in " \t"
            body = line.strip()
            if " = " in body:
                left, ret = body.split(" = ", 1)
            elif body.endswith("= <void>"):
                left, ret = body[: body.rfind("=")], "<void>"
            else:
                # e.g. "... = 0" with padding already collapsed by strip()
                left, _, ret = body.rpartition("=")
            left = left.strip()
            ret = ret.strip()
            name = left.split("(", 1)[0].strip()
            argstr = left[left.find("(") + 1: left.rfind(")")]
            try:
                args = ast.literal_eval(argstr) if argstr.strip() else []
            except Exception:
                args = [a.strip() for a in argstr.split(",")]
            args = [str(a) for a in args]
            if name in ("os_malloc", "os_calloc", "os_realloc", "os_free"):
                events.append(canon_public(name, args, ret))
            elif name == "brk":
                events.append("brk %s = %s" % (norm_token(args[0]), norm_token(ret)))
            elif name == "mmap":
                events.append("mmap len=%s = %s" % (args[1].strip(), norm_token(ret)))
            elif name == "munmap":
                events.append("munmap %s len=%s" % (norm_token(args[0]), args[1].strip()))
    return events


def canon_public(name, args, ret):
    if name in ("os_malloc", "os_calloc"):
        a = [x.strip() for x in args]            # plain sizes
    else:
        a = [norm_token(x) for x in args]        # ptr tokens / sizes
    return "%s(%s) = %s" % (name, ",".join(a), norm_token(ret))


def parse_mine(path):
    events = []
    with open(path) as f:
        start = None
        name = None
        args = None
        for raw in f:
            line = raw.rstrip("\n")
            if not line:
                continue
            parts = line.split("|")
            tag = parts[0]
            if tag == "P":
                name = parts[1]
                args = parts[2:]
                start = len(events)
            elif tag == "S":
                kind = parts[1]
                if kind == "brk":
                    events.append("brk %s = %s" % (norm_token(parts[2]), norm_token(parts[3])))
                elif kind == "mmap":
                    events.append("mmap len=%s = %s" % (parts[2], norm_token(parts[3])))
                elif kind == "munmap":
                    events.append("munmap %s len=%s" % (norm_token(parts[2]), parts[3]))
            elif tag == "R":
                events.insert(start, canon_public(name, args, parts[1]))
    return events


def main():
    name, ref_path, mine_path = sys.argv[1], sys.argv[2], sys.argv[3]
    ref = parse_ref(ref_path)
    mine = parse_mine(mine_path)
    if ref == mine:
        print("PASS  %s  (%d events)" % (name, len(ref)))
        return 0
    print("FAIL  %s" % name)
    diff = difflib.unified_diff(mine, ref, fromfile="mine", tofile="ref", lineterm="")
    shown = 0
    for d in diff:
        print("   " + d)
        shown += 1
        if shown > 40:
            print("   ...")
            break
    return 1


if __name__ == "__main__":
    sys.exit(main())
