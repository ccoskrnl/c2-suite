# e2e_screen.py — 阶段 2 验收：远程桌面拆成独立 agent
#
# 验证：
#   1) full 与 screen **两个模块同机共存**：各自命名空间、各自标签/能力位
#   2) screen 模块**直连 C2** 并独立提供远程桌面指令：
#        0x1001 截屏（GDI+WIC 经能力注册表）→ 合法 JPEG
#        0x1002 屏幕流 开/停（ratctl raw / stop）
#   3) full 模块**已不再包含**屏幕指令（0x1001 返回未知指令 status=1）
#
# 前置：
#   build/agent-full/bin/RatAgent.exe          ← 全量模块（module=full）
#   build/agents/screen/exe/RatAgent.screen.exe ← 屏幕模块（module=screen）
#   build/bin/c2server.exe / ratctl.exe
import os
import queue
import shutil
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C2 = os.path.join(ROOT, "build", "bin", "c2server.exe")
RATCTL = os.path.join(ROOT, "build", "bin", "ratctl.exe")
FULL = os.path.join(ROOT, "build", "agent-full", "bin", "RatAgent.exe")
SCREEN = os.path.join(ROOT, "build", "agents", "screen", "exe", "RatAgent.screen.exe")
WORK = os.path.join(ROOT, "build", "e2e", "screen")
C2_LOG = os.path.join(WORK, "c2server.log")
SHOT = os.path.join(WORK, "screen_shot.jpg")

ok = True


def check(name, cond, detail=""):
    global ok
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
    if not cond:
        ok = False


def read(p):
    try:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def online_ids(log):
    """从 C2 日志里取 (agent_id, 该行) 列表。"""
    out = []
    for line in log.splitlines():
        if " online from " in line:
            for tok in line.replace("[", " ").replace("]", " ").split():
                if len(tok) == 32 and all(c in "0123456789abcdef" for c in tok):
                    out.append((tok, line))
    return out


class Shell:
    def __init__(self):
        self.proc = subprocess.Popen([RATCTL, "-s", "127.0.0.1:4444"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, encoding="utf-8",
                                     errors="replace", bufsize=1, cwd=ROOT)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.proc.stdout:
            self.q.put(line)

    def drain(self):
        out = ""
        while True:
            try:
                out += self.q.get_nowait()
            except queue.Empty:
                break
        return out

    def send(self, cmds, wait):
        for c in cmds:
            self.proc.stdin.write(c + "\n")
            self.proc.stdin.flush()
        time.sleep(wait)
        return self.drain()

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()


def main():
    for p in (C2, RATCTL, FULL, SCREEN):
        check(f"prereq {os.path.relpath(p, ROOT)}", os.path.exists(p))
    if not ok:
        print("== RESULT: PREREQ MISSING")
        return 1

    shutil.rmtree(WORK, ignore_errors=True)
    dir_full = os.path.join(WORK, "run-full")
    dir_screen = os.path.join(WORK, "run-screen")
    os.makedirs(dir_full, exist_ok=True)
    os.makedirs(dir_screen, exist_ok=True)

    srv = a_full = a_screen = sh = sh_stream = None
    try:
        with open(C2_LOG, "w", encoding="utf-8", errors="replace") as f:
            srv = subprocess.Popen([C2], stdout=f, stderr=subprocess.STDOUT)
        time.sleep(1)
        dn = subprocess.DEVNULL
        a_full = subprocess.Popen([FULL, "127.0.0.1", "4444", "-tcp"],
                                  cwd=dir_full, stdout=dn, stderr=dn)
        a_screen = subprocess.Popen([SCREEN, "127.0.0.1", "4444", "-tcp"],
                                    cwd=dir_screen, stdout=dn, stderr=dn)

        t0 = time.time()
        while time.time() - t0 < 30:
            if len(online_ids(read(C2_LOG))) >= 2:
                break
            time.sleep(0.5)
        log = read(C2_LOG)
        ids = online_ids(log)
        check("两个模块都上线", len(ids) >= 2, f"online={len(ids)}")

        # 1) 命名空间与标签
        check("full 生成 full.id", os.path.exists(os.path.join(dir_full, "full.id")))
        check("screen 生成 screen.id", os.path.exists(os.path.join(dir_screen, "screen.id")))
        check("两个模块 id 不同", len({i for i, _ in ids}) >= 2)
        check("full 标签", 'os="Windows (full)"' in log and "plugins=0x00000001" in log)
        check("screen 标签", 'os="Windows (screen)"' in log and "plugins=0x00000002" in log)

        sid = next((i for i, l in ids if "(screen)" in l), "")
        fid = next((i for i, l in ids if "(full)" in l), "")
        check("识别出 screen / full 两个会话", bool(sid) and bool(fid), f"{sid[:8]} / {fid[:8]}")
        if not (sid and fid):
            raise SystemExit(1)

        sh = Shell()   # 干净会话：不发起屏幕流，因此不会收到 4445 转发的帧
        sh_stream = Shell()  # 发流会话：由它发起 0x1002，C2 会把帧转给它（它会收到大量帧）

        # 2) screen 模块：0x1001 截屏
        out = sh.send([f"screenshot {sid} {SHOT.replace(os.sep, '/')}"], 20)
        jpeg_ok = False
        if os.path.exists(SHOT):
            with open(SHOT, "rb") as f:
                head = f.read(2)
            jpeg_ok = head == b"\xff\xd8" and os.path.getsize(SHOT) > 1024
        check("screen 模块 0x1001 截屏（GDI+WIC）", jpeg_ok,
              f"{os.path.getsize(SHOT)} bytes" if os.path.exists(SHOT) else repr(out[-90:]))

        # 3) screen 模块：0x1002 停流（此时没有流在跑，验证指令通路）
        #    同时验证 ctl 侧**按模块标签**解析目标（不必手抄 32 位 id）
        out = sh.send(["use screen", "stop"], 12)
        check("按模块标签 use screen 生效", f"switched to agent {sid}" in out, repr(out[-140:]))
        check("screen 模块 0x1002 停流指令可用", "screen stream stopped" in out, repr(out[-120:]))

        # 4) 由"发流会话"开流；C2 会把帧转发给发起 0x1002 的那条 ctl 连接
        sh_stream.send(["use screen"], 3)
        out = sh_stream.send(["raw 1002 010a4664"], 10)
        check("screen 模块 0x1002 开流", "screen stream started" in out, repr(out[-120:]))

        # 5) 数据面：C2 日志应出现 4445 中继的帧（证明屏幕流真的在跑）
        t0 = time.time()
        while time.time() - t0 < 20:
            if "relay frame" in read(C2_LOG):
                break
            time.sleep(0.5)
        check("4445 中继收到帧（数据面打通）", "relay frame" in read(C2_LOG),
              next((l for l in read(C2_LOG).splitlines() if "relay frame" in l), ""))

        # 6) 从"干净会话"停流（命令按 agent_id 路由，与哪条连接发起无关）
        out = sh.send(["stop"], 12)
        check("从另一个会话也能停流", "screen stream stopped" in out, repr(out[-120:]))
        t0 = time.time()
        while time.time() - t0 < 15:
            if "removed stream connection" in read(C2_LOG):
                break
            time.sleep(0.5)
        check("停流后 4445 连接被移除", "removed stream connection" in read(C2_LOG))

        # 7) full 模块：屏幕指令**已移除**（同样用模块标签切换）
        out = sh.send(["use full", "raw 1001"], 10)
        check("full 模块已无 0x1001（未知指令 status=0x01）",
              "status : 0x01" in out, repr(out[-120:]))
        out = sh.send(["raw 1002 01"], 10)
        check("full 模块已无 0x1002（未知指令 status=0x01）",
              "status : 0x01" in out, repr(out[-120:]))
    finally:
        if sh:
            sh.close()
        if sh_stream:
            sh_stream.close()
        for proc in (a_full, a_screen):
            if proc and proc.poll() is None:
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        if srv:
            srv.terminate()
            try:
                srv.wait(timeout=5)
            except subprocess.TimeoutExpired:
                srv.kill()

    print("== RESULT:", "ALL PASS" if ok else "FAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
