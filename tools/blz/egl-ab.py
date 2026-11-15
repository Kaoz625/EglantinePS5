#!/usr/bin/env python3
"""egl-ab.py - install one EglantinePS5 build on a PS5 and measure a game on it (claude-egl-rebuild, 15 Nov 2026).

    egl-ab.py install <console-ip> <unzipped PPSA98515 folder> <tag>
        Swaps the FILES inside /data/homebrew/PPSA98515 (never the folder: a nullfs mount keeps the old
        folder object). The live eboot.bin is kept as eboot.bin.<live-tag> once (sha-named if no tag file).
        Every uploaded file is read back over websrv :8080 and its sha256 compared before it is renamed in.
        Refuses while any eboot.bin runs.
    egl-ab.py run <console-ip> <tag> <game> <seconds> [--script FILE]
        game = mkw | oot | dkb | <16-hex title id>. Writes dev-settings.txt (rom=<id> unless mkw), launches
        PPSA98515 with appmove.elf, records klog :3232 to ~/.blazing/egl-rebuild/<tag>-<game>-<time>.klog, plays
        the --script (lines "<at_s> <mask> <ms> [lx ly rx ry]", ids are made here), pulls game-frame.ppm every 30 s,
        then closes the app (pldmgr process_kill) and prints the fps / memory summary.
    egl-ab.py summary <klog file>
The caller holds the console claim (ps5-claim) and renews it.
"""
import urllib.parse, ftplib, hashlib, io, json, os, re, socket, subprocess, sys, threading, time, urllib.request

HOME = os.path.expanduser("~")
OUT = os.path.join(HOME, ".blazing/egl-rebuild")
APP = "/data/homebrew/PPSA98515"
RUN = os.path.join(HOME, "NYCT/ps5-linux/tools/run-payload.sh")
APPMOVE = os.path.join(HOME, "NYCT/blazing-ps5/tools/payloads/appmove/appmove.elf")
NFS = "/nfs/192.168.1.130/switch/"
GAMES = {"mkw": "Mario Kart World [v1.8.1][0400C3F00006E000].nup",
         "oot": "The Legend of Zelda Ocarina of Time (v1.1.1) [0400486024DEC000].nup",
         "dkb": "Donkey Kong Bananza [040074A01BF12000]",
         "splatoon": "Splatoon Raiders [0400380020056000] [packed].nup",
         "starfox": "Star Fox [0400E88025B1A000] [packed].nup"}


def log(m):
    print(time.strftime("%H:%M:%S"), m, flush=True)


def ftp(ip):
    f = ftplib.FTP()
    f.connect(ip, 2121, timeout=60)
    f.login()
    return f


def web_get(ip, path, timeout=120):
    with urllib.request.urlopen(f"http://{ip}:8080/fs{urllib.request.quote(path)}", timeout=timeout) as r:
        return r.read()


def eboot_pids(ip):
    with urllib.request.urlopen(f"http://{ip}:8084/processes_list", timeout=10) as r:
        return [p["pid"] for p in json.loads(r.read())["processes"] if p["name"] == "eboot.bin"]


def kill_app(ip):
    for pid in eboot_pids(ip):
        req = urllib.request.Request(f"http://{ip}:8084/process_kill?pid={pid}", method="POST",
                                     headers={"X-PLDMGR-Request": "1"})
        with urllib.request.urlopen(req, timeout=10) as r:
            log(f"process_kill {pid}: {r.status} {r.read()[:80]!r}")


def size_of(f, path):
    try:
        return f.size(path)
    except ftplib.error_perm:
        return None


def install(ip, src, tag):
    if eboot_pids(ip):
        sys.exit("an eboot.bin runs - refusing to install")
    f = ftp(ip)
    f.voidcmd("TYPE I")
    files = []
    for root, _, names in os.walk(src):
        for n in names:
            full = os.path.join(root, n)
            files.append((full, os.path.relpath(full, src).replace(os.sep, "/")))
    live_tag = (web_get(ip, f"{APP}/blz-build-tag.txt").decode().strip() if size_of(f, f"{APP}/blz-build-tag.txt")
                else "unknown")
    old = size_of(f, f"{APP}/eboot.bin")
    if old and size_of(f, f"{APP}/eboot.bin.{live_tag}") is None:
        f.rename(f"{APP}/eboot.bin", f"{APP}/eboot.bin.{live_tag}")   # kept beside it, renamed back below if no new one
        f.storbinary(f"STOR {APP}/eboot.bin", io.BytesIO(b""))         # placeholder, replaced in the loop
        log(f"live eboot.bin ({old} B) kept as eboot.bin.{live_tag}")
    changed = 0
    for full, rel in sorted(files, key=lambda x: x[1] == "eboot.bin"):   # eboot.bin last
        data = open(full, "rb").read()
        dest = f"{APP}/{rel}"
        if size_of(f, dest) == len(data) and hashlib.sha256(web_get(ip, dest)).digest() == hashlib.sha256(data).digest():
            continue
        parts = rel.split("/")[:-1]
        for i in range(len(parts)):
            try:
                f.mkd(f"{APP}/" + "/".join(parts[: i + 1]))
            except ftplib.error_perm:
                pass
        f.storbinary(f"STOR {dest}.part", io.BytesIO(data), blocksize=1 << 20)
        back = web_get(ip, f"{dest}.part")
        if hashlib.sha256(back).digest() != hashlib.sha256(data).digest():
            sys.exit(f"read-back mismatch: {rel} ({len(back)} vs {len(data)})")
        try:
            f.delete(dest)
        except ftplib.error_perm:
            pass
        f.rename(f"{dest}.part", dest)
        changed += 1
        log(f"placed {rel} ({len(data)} B, sha checked)")
    f.storbinary(f"STOR {APP}/blz-build-tag.txt", io.BytesIO(tag.encode() + b"\n"))
    want = os.path.getsize(os.path.join(src, "eboot.bin"))
    log(f"{changed} files changed; eboot.bin live {size_of(f, APP + '/eboot.bin')} B, build {want} B")
    f.quit()


def mounted_eboot_size(ip):
    f = ftp(ip)
    s = size_of(f, "/system_ex/app/PPSA98515/eboot.bin")
    f.quit()
    return s


def klog_reader(ip, path, stop):
    with open(path, "ab") as out:
        while not stop.is_set():
            try:
                s = socket.create_connection((ip, 3232), timeout=10)
                s.settimeout(5)
                while not stop.is_set():
                    try:
                        d = s.recv(65536)
                    except socket.timeout:
                        continue
                    if not d:
                        break
                    out.write(d)
                    out.flush()
            except OSError:
                time.sleep(2)


def put_text(ip, name, text):
    f = ftp(ip)
    f.storbinary(f"STOR {APP}/{name}", io.BytesIO(text.encode()))
    f.quit()


SEEN_CRASHES = set()


def run(ip, tag, game, seconds, script=None):
    os.makedirs(OUT, exist_ok=True)
    try:
        SEEN_CRASHES.update(e["name"] for e in json.loads(web_get(ip, "/data/eglantineps5/logs/?fmt=json")))
    except Exception:
        pass
    if eboot_pids(ip):
        sys.exit("an eboot.bin already runs - refusing to launch")
    rom = urllib.parse.quote(NFS + GAMES.get(game, game), safe="/")
    put_text(ip, "compat-input.txt", "")
    time.sleep(15)   # 15 Nov: DIGI5 dropped off the LAN when a launch followed a kill too closely (cause unproven)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(OUT, f"{tag}-{game}-{stamp}.klog")
    stop = threading.Event()
    th = threading.Thread(target=klog_reader, args=(ip, path, stop), daemon=True)
    th.start()
    time.sleep(1)
    out = subprocess.run([RUN, APPMOVE, "launch", "PPSA98515", "--rom=" + rom], capture_output=True, text=True,
                         env=dict(os.environ, PS5=ip, WAIT="12")).stdout
    log("launch: " + " | ".join(l.split("[appmove] ", 1)[1] for l in out.splitlines() if "[appmove] " in l))
    t0 = time.time()
    time.sleep(20)
    log(f"/system_ex/app/PPSA98515/eboot.bin = {mounted_eboot_size(ip)} B")
    steps = []
    if script:
        for line in open(script):
            p = line.split("#")[0].split()
            if p:
                steps.append((float(p[0]), " ".join(p[1:])))
    seq = int(time.time() * 1000)
    next_shot = 30
    while time.time() - t0 < seconds:
        el = time.time() - t0
        while steps and steps[0][0] <= el:
            seq += 1
            put_text(ip, "compat-input.txt", f"{seq} {steps.pop(0)[1]}\n")
        if el >= next_shot:
            next_shot += 30
            try:
                data = web_get(ip, "/data/eglantineps5/logs/game-frame.ppm", timeout=30)
                open(os.path.join(OUT, f"{tag}-{game}-{stamp}-{int(el):04d}.ppm"), "wb").write(data)
            except Exception:
                pass
        if el > 60 and not eboot_pids(ip):
            log(f"app ended by itself at {el:.0f} s")
            break
        time.sleep(0.5)
    if eboot_pids(ip):
        # TouchPad+L1 = the app's "back to the menu" chord: the game session ends normally and its logs are
        # written out (a kill lost the last ~4 min of heap.log on 15 Nov)
        seq += 1
        put_text(ip, "compat-input.txt", f"{seq} {0x100400} 600\n")
        time.sleep(15)
    time.sleep(20)   # a crash report is written a few seconds after the process is gone
    # stdout (EDEN_VULKAN_FRAME fps, MKW_MEMORY) goes to logs/heap.log; a crash renames it crash-<time>-heap.log
    logs = "/data/eglantineps5/logs"
    try:
        names = [e["name"] for e in json.loads(web_get(ip, logs + "/?fmt=json"))]
    except Exception:
        names = []
    crashes = sorted(n for n in names if n.startswith("crash-") and n.endswith(".txt") and n != "crash-index.txt"
                     and n not in SEEN_CRASHES)
    pulled = []
    for n in ["heap.log", "stderr.log"] + [c[:-4] + suffix for c in crashes for suffix in ("-heap.log", "-stderr.log", ".txt")]:
        try:
            data = web_get(ip, f"{logs}/{n}", timeout=60)
        except Exception:
            continue
        dest = path[:-5] + "." + n
        open(dest, "wb").write(data)
        pulled.append(dest)
    if eboot_pids(ip):
        kill_app(ip)
    time.sleep(3)
    stop.set()
    log(f"klog: {path}; crash reports: {crashes or 'none'}")
    heap = [p for p in pulled if p.endswith("-heap.log")] or [p for p in pulled if p.endswith(".heap.log")]
    for c in [p for p in pulled if p.endswith(".txt") and "crash-" in p]:
        print(open(c, errors="replace").read()[:700])
    for h in heap:
        print("==", os.path.basename(h))
        summary(h)


def summary(path):
    text = open(path, "rb").read().decode("utf-8", "replace")
    fps = [(float(m.group(2)), float(m.group(3))) for m in
           re.finditer(r"EDEN_(VULKAN|GAME)_FRAME frames=\d+ seconds=[\d.]+ fps=([\d.]+) worst_ms=([\d.]+)", text)]
    mem = re.findall(r"MKW_MEMORY seconds=(\d+).*?guest=(\d+).*?heap=(\d+)", text)
    bad = re.findall(r".*(OUT_OF_DEVICE_MEMORY|out of memory|unmapped address|EDEN_CRASH|guest fault|GPU hang|"
                     r"DEVICE_LOST|Fatal|panic).*", text, re.I)
    print(f"samples={len(fps)} (5 s each)")
    if fps:
        v = [f for f, _ in fps]
        body = [f for f in v if f > 1]
        print("fps per 5 s:", " ".join(f"{f:.0f}" for f in v))
        if body:
            s = sorted(body)
            print(f"fps over {len(body)} samples >1: median {s[len(s)//2]:.1f}  min {s[0]:.1f}  max {s[-1]:.1f}  "
                  f"mean {sum(s)/len(s):.1f}; worst frame {max(w for _, w in fps):.0f} ms")
    if mem:
        last = mem[-1]
        print(f"MKW_MEMORY last: seconds={last[0]} guest={int(last[1])/2**20:.0f} MiB heap={int(last[2])/2**20:.0f} MiB "
              f"(peak guest {max(int(m[1]) for m in mem)/2**20:.0f} MiB, heap {max(int(m[2]) for m in mem)/2**20:.0f} MiB)")
    for m in re.findall(r"EDEN_LOADING_DONE[^\n]*", text)[:3]:
        print(m)
    print(f"error lines: {len(bad)}")
    for line in re.findall(r"[^\n]*(?:OUT_OF_DEVICE_MEMORY|out of memory|unmapped address|EDEN_CRASH|DEVICE_LOST)[^\n]*", text, re.I)[:6]:
        print("  ", line[:220])


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "install":
        install(a[1], a[2], a[3])
        print("mounted eboot (only meaningful after the next launch):", mounted_eboot_size(a[1]))
    elif a[0] == "run":
        scr = a[a.index("--script") + 1] if "--script" in a else None
        run(a[1], a[2], a[3], int(a[4]), scr)
    elif a[0] == "summary":
        summary(a[1])
