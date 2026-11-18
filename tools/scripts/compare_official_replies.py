"""Compare the official server's replies (from extract_official_api.py) with our C# server's canned replies.

Official reply = [header map, body]; our api-responses.json stores the body only. For each official endpoint:
  - missing     : our server has no route for it
  - dynamic     : our server builds the reply in code (not compared here; listed for review)
  - same shape  : body has the same structure (list lengths and value types, recursively)
  - differs     : structure differs; the first difference is shown
Usage: python compare_official_replies.py <official api reference dir> <BreakerServer project dir> <out .md>
"""
import json
import os
import re
import sys
from collections import Counter

ref_dir, proj, out_md = sys.argv[1:4]
api_cs = open(os.path.join(proj, "Endpoints", "ApiEndpoints.cs"), encoding="utf-8").read()
catalog = json.load(open(os.path.join(proj, "Assets", "api-responses.json"), encoding="utf-8"))

static = dict(re.findall(r'Static\("(/[^"]+)",\s*"([^"]+)"\)', api_cs))          # path -> catalog name
routes = set(p.rstrip("/") for p in re.findall(r'Map(?:Post|Get)\("(/[^"]+)"', api_cs))
for f in os.listdir(os.path.join(proj, "Endpoints")):
    if f.endswith(".cs"):
        routes |= set(p.rstrip("/") for p in re.findall(r'Map(?:Post|Get)\("(/[^"]+)"',
                                                       open(os.path.join(proj, "Endpoints", f), encoding="utf-8").read()))
routes |= set(static)
# Routes registered in a loop (foreach over a list of "/area/name" strings, e.g. the [0] acknowledgements).
routes |= set(re.findall(r'^\s*"(/[a-z0-9_]+/[a-z0-9_/]+)",?\s*(?://.*)?$', api_cs, re.M))


def shape(v, depth=0):
    if depth > 6:
        return "..."
    if isinstance(v, list):
        if not v:
            return "[]"
        if len(v) > 4 and all(type(x) == type(v[0]) for x in v):   # homogeneous list: describe one element
            return "[%s x%d]" % (shape(v[0], depth + 1), len(v))
        return "[" + ", ".join(shape(x, depth + 1) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ", ".join("%s:%s" % (k, shape(x, depth + 1)) for k, x in sorted(v.items())) + "}"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, int):
        return "int"
    if isinstance(v, float):
        return "float"
    if isinstance(v, str):
        return "str"
    return "null" if v is None else type(v).__name__


def first_diff(a, b, path="body"):
    if type(a) != type(b) and not (isinstance(a, (int, float)) and isinstance(b, (int, float))):
        return "%s: official %s, ours %s" % (path, shape(a), shape(b))
    if isinstance(a, list):
        # A list of records (every element a list) is a collection: how many entries an account owns differs
        # (main account vs new account), so compare one entry's layout, not the count.
        if a and b and all(isinstance(x, list) for x in a + b):
            return first_diff(a[0], b[0], path + "[entry]")
        if len(a) != len(b) and not (len(a) > 4 or len(b) > 4):
            return "%s: official has %d values, ours %d" % (path, len(a), len(b))
        for i, (x, y) in enumerate(zip(a, b)):
            d = first_diff(x, y, "%s[%d]" % (path, i))
            if d:
                return d
        if len(a) != len(b) and (len(a) <= 4 and len(b) <= 4):
            return "%s: length %d vs %d" % (path, len(a), len(b))
    if isinstance(a, dict):
        if set(a) != set(b):
            return "%s: keys differ (official only: %s; ours only: %s)" % (
                path, sorted(set(a) - set(b))[:5], sorted(set(b) - set(a))[:5])
    return None


calls = Counter()
example = {}
with open(os.path.join(ref_dir, "all_calls.jsonl"), encoding="utf-8") as f:
    for line in f:
        c = json.loads(line)
        ep = "/" + "/".join(c["path"].split("?")[0].split("/")[3:])
        calls[ep] += 1
        r = c.get("reply")
        if ep not in example and isinstance(r, list) and len(r) >= 2 and isinstance(r[0], dict):
            example[ep] = r[1] if len(r) == 2 else r[1:]

rows = []
for ep in sorted(calls, key=lambda e: (-calls[e], e)):
    if ep not in routes:
        status, note = "missing", "no route in our server"
    elif ep in static and static[ep] in catalog:
        ours = catalog[static[ep]]
        off = example.get(ep)
        if off is None:
            status, note = "no official body", "official reply could not be decoded"
        else:
            d = first_diff(off, ours)
            status, note = ("same shape", "") if d is None else ("differs", d)
    else:
        status, note = "dynamic", "reply built in code: compare by hand"
    rows.append((ep, calls[ep], status, note))

summary = Counter(r[2] for r in rows)
with open(out_md, "w", encoding="utf-8") as f:
    f.write("# Official server calls vs our C# server\n\n")
    f.write("From %d decoded official calls (Wireshark + TLS key log, May-Oct 2026). " % sum(calls.values()))
    f.write("Shape = list lengths and value types, recursively; values themselves are not compared.\n\n")
    f.write("| Status | Calls |\n|---|---|\n" + "".join("| %s | %d |\n" % kv for kv in summary.most_common()) + "\n")
    f.write("| Endpoint | Seen | Status | Note |\n|---|---|---|---|\n")
    for ep, n, st, note in rows:
        f.write("| `%s` | %d | %s | %s |\n" % (ep, n, st, note.replace("|", "/")))
print(dict(summary))
