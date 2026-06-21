print("=== OS STRESS TEST START ===")

# fake wrappers (your OS should expose these somehow)
open_file = open
read_file = lambda p: open(p, "rb").read()
write_file = lambda p, d: open(p, "wb").write(d)

# ---------- FILE TEST ----------
print("[1] file test")
for i in range(10):
    p = "/stress_" + str(i)
    write_file(p, b"X" * (i + 1) * 1024)

for i in range(10):
    p = "/stress_" + str(i)
    d = read_file(p)
    print("read", i, len(d))

# ---------- CPU TEST ----------
print("[2] cpu test")
x = 0
for i in range(200000):
    x ^= (i * 1103515245) & 0xFFFFFFFF
print("cpu done", x)

# ---------- MEMORY TEST ----------
print("[3] mem test")
arr = []
for i in range(50):
    arr.append(bytearray(b"A" * 50000))
print("mem blocks:", len(arr))

# ---------- CHAOS ----------
print("[4] chaos")
for i in range(50):
    try:
        p = "/stress_" + str(i % 10)
        write_file(p, b"".join([bytes([i % 255]) for _ in range(1000)]))
    except:
        pass

print("=== DONE ===")