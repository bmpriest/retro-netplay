#!/usr/bin/env python3
"""Summarise cc3264 cell output: is spruce's 32-bit build of a core
netplay-compatible with its 64-bit build?  judge.py <out dir> [--tsv]"""
import os, re, sys

def rd(p):
    try:
        return open(p, errors="replace").read()
    except OSError:
        return ""

def det(path):
    t = rd(path)
    ver = re.search(r"^core: (.*)$", t, re.M)
    size = re.search(r"^state size: (\d+)", t, re.M)
    cps = re.findall(r"^frame +(\d+)  ([0-9a-f]{8})", t, re.M)
    return (ver.group(1) if ver else None, int(size.group(1)) if size else None,
            [(int(f), h) for f, h in cps], t)

def sc(path):
    t = rd(path)
    g = lambda r: (re.search(r, t, re.M) or [None, None])[1]
    return dict(size=g(r"^state +: (\d+) bytes"), after=g(r"^after +\d+ *: ([0-9a-f]{8})"),
                rt=g(r"^roundtrip: (.*)$"), ser=g(r"^serprobe : (identical|DIFFERS[^-]*)"),
                amp=g(r"^amplify  : (identical|DIVERGED[^-]*)"), err=t.strip().splitlines()[-1] if t.strip() else "")

def bindiff(a, b):
    try:
        x, y = open(a, "rb").read(), open(b, "rb").read()
    except OSError:
        return None
    if len(x) != len(y):
        return "size"
    d = [i for i in range(len(x)) if x[i] != y[i]]
    return (len(d), d[0] if d else None)

def cell(o):
    r = {"name": os.path.basename(o)}
    v32, s32, c32, t32 = det(f"{o}/det32.a.log")
    v64, s64, c64, t64 = det(f"{o}/det64.a.log")
    r["v32"], r["v64"] = v32, v64
    r["size"] = f"{s32}" if s32 == s64 else f"{s32}/{s64}"
    if not c32 or not c64:
        bad = t32 if not c32 else t64
        r["verdict"] = "NO RUN"
        r["note"] = ("32: " if not c32 else "64: ") + (bad.strip().splitlines() or ["no output"])[-1][:90]
        return r
    _, _, c32b, _ = det(f"{o}/det32.b.log")
    _, _, c64b, _ = det(f"{o}/det64.b.log")
    _, _, c32c, _ = det(f"{o}/det32.clock.log")
    _, _, c64c, _ = det(f"{o}/det64.clock.log")
    r["self32"] = c32 == c32b
    r["self64"] = c64 == c64b
    r["clock"] = (c32 == c32c) and (c64 == c64c)
    n = min(len(c32), len(c64))
    match = 0
    first = None
    for i in range(n):
        if c32[i][1] == c64[i][1]:
            match += 1
        elif first is None:
            first = c32[i][0]
    r["cross"] = f"{match}/{n}"
    r["first_diff"] = first
    bd = bindiff(f"{o}/det32.a.bin", f"{o}/det64.a.bin")
    r["final_diff"] = bd
    a32, a64 = sc(f"{o}/sc32.log"), sc(f"{o}/sc64.log")
    r["rt32"], r["rt64"] = a32["rt"], a64["rt"]
    r["ser32"], r["ser64"] = a32["ser"], a64["ser"]
    r["amp32"], r["amp64"] = a32["amp"], a64["amp"]
    r["sc_same_save"] = a32["after"] == a64["after"] and a32["after"] is not None
    x = {k: sc(f"{o}/x{k}.log") for k in ("32-32", "32-64", "64-32", "64-64")}
    r["xload"] = {k: (v["after"] or ("ERR " + v["err"][:60])) for k, v in x.items()}
    from32 = x["32-32"]["after"] is not None and x["32-32"]["after"] == x["32-64"]["after"]
    from64 = x["64-64"]["after"] is not None and x["64-64"]["after"] == x["64-32"]["after"]
    r["xload_ok"] = (from32, from64)
    det_ok = match == n and r["self32"] and r["self64"]
    if s32 != s64:
        r["verdict"] = "INCOMPATIBLE (state size)"
    elif det_ok and from32 and from64:
        r["verdict"] = "COMPATIBLE"
    elif det_ok:
        r["verdict"] = "SAME EMULATION, cross-load differs"
    elif not (r["self32"] and r["self64"]):
        r["verdict"] = "NONDETERMINISTIC"
    elif from32 and from64:
        r["verdict"] = "LOADS MATCH, cold boot drifts"
    else:
        r["verdict"] = "INCOMPATIBLE (diverges)"
    return r

def main():
    root = sys.argv[1]
    rows = [cell(os.path.join(root, d)) for d in sorted(os.listdir(root))
            if os.path.isfile(os.path.join(root, d, "done"))]
    for r in rows:
        print(f"{r['name']:26} {r['verdict']}")
        if "cross" not in r:
            print(f"    {r.get('note','')}")
            continue
        print(f"    32={r['v32']} | 64={r['v64']} | size={r['size']}")
        print(f"    det 32v64 {r['cross']} checkpoints (first diff @{r['first_diff']}), final-state bytes differ={r['final_diff']}, "
              f"self32={r['self32']} self64={r['self64']} clock-free={r['clock']}")
        print(f"    roundtrip 32:{r['rt32']} | 64:{r['rt64']}; serprobe 32:{r['ser32']} 64:{r['ser64']}; amplify 32:{r['amp32']} 64:{r['amp64']}")
        print(f"    cross-load from32 ok={r['xload_ok'][0]} from64 ok={r['xload_ok'][1]}  {r['xload']}")

if __name__ == "__main__":
    main()
