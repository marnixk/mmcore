import glob, os, re

files = sorted(glob.glob("docs/help/*.txt"))
for f in files:
    content = open(f, encoding="utf-8").read()
    lines = content.splitlines()
    ex_idx = -1
    for i, line in enumerate(lines):
        if re.match(r"^Examples?:", line.strip()):
            ex_idx = i
            break
    if ex_idx >= 0:
        ex_lines = lines[ex_idx+1:]
        lns = []
        for el in ex_lines:
            m = re.match(r"^\s*(\d{1,5})\s+([A-Za-z\?\'\"].*)", el)
            if m:
                lns.append((el, m.group(1), m.group(2)))
        if lns:
            print(f"=== {os.path.basename(f)}: has line numbers ===")
            for el, num, rest in lns:
                print(f"   {el}")
    else:
        kind = "command"
        for l in lines[:5]:
            if l.startswith("kind:"):
                kind = l.split(":", 1)[1].strip()
        if kind != "page":
            print(f"*** {os.path.basename(f)}: NO EXAMPLE SECTION! ***")
