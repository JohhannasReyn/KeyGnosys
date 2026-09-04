"""Judge chord attempts with strict ONE-TO-ONE pairing.

The earlier analysis let a single CapsLock press act as partner for several J
presses, inventing 21 attempts out of 10. Here each CapsLock is consumed by at
most one J, nearest-first, so an attempt is a genuine pair or it is not counted.
"""
import sys
J, CAPS = 0x4A, 0x14
WINDOW = float(sys.argv[2]) if len(sys.argv) > 2 else 80.0

rows = []
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.strip()
    if not line:
        continue
    t, vk, inj, dn = line.split(",")
    rows.append((float(t), int(vk), inj == "1", dn))

jd = [r for r in rows if r[1] == J and not r[2] and r[3] == "D"]
cd = [r for r in rows if r[1] == CAPS and not r[2] and r[3] == "D"]
ji = [r for r in rows if r[1] == J and r[2] and r[3] == "D"]

# All candidate pairs within the window, nearest first, each side used once.
pairs = sorted(((abs(c[0] - j[0]), j, c) for j in jd for c in cd
                if abs(c[0] - j[0]) <= WINDOW), key=lambda x: x[0])
usedJ, usedC, attempts = set(), set(), []
for _, j, c in pairs:
    if id(j) in usedJ or id(c) in usedC:
        continue
    usedJ.add(id(j)); usedC.add(id(c))
    attempts.append((j, c))
attempts.sort(key=lambda p: p[0][0])

print(f"pairing window +/-{WINDOW:.0f} ms, one-to-one")
print(f"physical J={len(jd)}  CapsLock={len(cd)}  injected J={len(ji)}\n")
print("   delta_t (Caps-J) | replay? | verdict")
usedI, deltas, leaks = set(), [], 0
for j, c in attempts:
    d = c[0] - j[0]
    deltas.append(d)
    hit = [i for i in ji if 0 < i[0] - j[0] < 400 and id(i) not in usedI]
    if hit:
        usedI.add(id(min(hit, key=lambda x: x[0]))); leaks += 1
        print(f"     {d:+8.1f} ms      |  YES    | LEAK - letter typed")
    else:
        print(f"     {d:+8.1f} ms      |  no     | OK - chord")
print()
print(f"  attempts: {len(attempts)}   ok: {len(attempts)-leaks}   leaks: {leaks}")
if deltas:
    s = sorted(deltas)
    print(f"  delta_t: min={s[0]:+.1f}  median={s[len(s)//2]:+.1f}  max={s[-1]:+.1f}")
    print(f"  worst |delta| = {max(abs(x) for x in s):.1f} ms")
    inside = sum(1 for x in s if abs(x) <= 25)
    print(f"  attempts with |delta| <= 25 ms: {inside}/{len(s)}  "
          f"(i.e. would still resolve at grace_ms=25)")
