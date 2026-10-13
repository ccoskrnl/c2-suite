# e2e_template.py — 模块模板验收（阶段 1）
#
# 验证"复制模板即可产出新模块，不必改 core"：
#   1) 模板产物能独立编译（EXE + DLL）
#   2) 它以**自己的模块命名空间**上线：<module>.id、C2 侧标签 Windows (template)、能力位
#   3) 它登记的自定义指令能用（ratctl raw 2001 deadbeef → demo-echo）
#   4) core 内置指令在模板里同样可用（sysinfo）
#
# 前置：
#   build/agents/template/exe/RatAgent.template.exe   ← 模板工程 Release 构建
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
AGENT = os.path.join(ROOT, "build", "agents", "template", "exe", "RatAgent.template.exe")
WORK = os.path.join(ROOT, "build", "e2e", "template")
RUN = os.path.join(WORK, "run")
C2_LOG = os.path.join(WORK, "c2server.log")
MODULE = "template"

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
        check("模板模块上线", len(aid) == 32, aid)
        check("模块命名空间生效（CWD 下 template.id）",
              os.path.exists(os.path.join(RUN, f"{MODULE}.id")),
              f"{MODULE}.id")
        check("C2 侧标签正确", 'os="Windows (template)"' in log)
        check("能力位正确（仅系统类 bit0）", "plugins=0x00000001" in log)
        if not aid:
            raise SystemExit(1)

        sh = Shell()
        out = sh.send(["use " + aid], 4)
        check("use 切换成功", aid[:8] in out, repr(out[-80:]))

        # 3) 模板自己登记的指令（0x2001 回显）
        out = sh.send(["raw 2001 deadbeef"], 12)
        check("自定义指令经注册表可用", "demo-echo: de ad be ef" in out, repr(out[-140:]))

        # 4) core 内置指令仍然可用
        out = sh.send(["sysinfo"], 12)
        check("core 内置指令仍可用", "OS:" in out and "HOST:" in out, repr(out[-80:]))
    finally:
        if sh:
            sh.close()
        if agent and agent.poll() is None:
            agent.kill()
            try:
                agent.wait(timeout=5)
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
