"""Count RTTI type names by outermost namespace; optionally grep names."""
import sys, re, collections
raw = open(sys.argv[1], "rb").read()
names = sorted({m.decode() for m in re.findall(rb"\.\?A[VUW]([A-Za-z0-9_@?$<>]+?)@@", raw)})
ns = collections.Counter(n.split("@")[-1] if "@" in n else "(global)" for n in names)
print(len(names), "types;", ", ".join(f"{k}:{v}" for k, v in ns.most_common(40)))
if len(sys.argv) > 2:
    pat = re.compile(sys.argv[2], re.I)
    hits = [n for n in names if pat.search(n)]
    print(f"{len(hits)} match {sys.argv[2]!r}:", " ".join(hits[:120]))
