# e2e_modules.py — 模块命名空间与同机共存验收（阶段 0）
#
# 验证：**同一份 core 源码**编出的两个"模块"（full / probe）在同一台机器上并发运行：
#   1) 各自生成自己的 <module>.id（CWD 下），互不覆盖、互不误触发
#   2) 两个会话都能上线，agent_id 不同（不会因 id 冲突互相踢下线）
#   3) C2 侧能看到模块标签（os 字段）与能力位（plugins 位图）
#
# 为什么要用 -tcp 而不是 beacon：beacon 信道的会话在服务端是**硬编码**
# `OSInfo="beacon-agent"` 的（见 server/cmd/c2server/beacon.go），握手元数据（os/plugins）
# 只走 TCP 长连接的 HELLO —— 模块标签在 beacon 上是个已知缺口（待单独补协议）。
#
# 前置（同一工程编两次）：
#   build/agent-full/bin/RatAgent.exe    ← 默认（module=full,  caps=sys  = 0x1）
#   build/agent-probe/bin/RatAgent.exe   ← -p:RatAgentModuleId=probe -p:RatAgentModuleCaps=(c2::kCapRemote)  = 0x2
import os
import shutil
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C2 = os.path.join(ROOT, "build", "bin", "c2server.exe")
RATCTL = os.path.join(ROOT, "build", "bin", "ratctl.exe")
FULL = os.path.join(ROOT, "build", "agent-full", "bin", "RatAgent.exe")
PROBE = os.path.join(ROOT, "build", "agent-probe", "bin", "RatAgent.exe")
WORK = os.path.join(ROOT, "build", "e2e", "modules")
C2_LOG = os.path.join(WORK, "c2server.log")

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


def hex_of_id_file(path):
    try:
        with open(path, "rb") as f:
            b = f.read(16)
        return b.hex() if len(b) == 16 else ""
    except OSError:
        return ""


def main():
    for p in (C2, RATCTL, FULL, PROBE):
        check(f"prereq {os.path.relpath(p, ROOT)}", os.path.exists(p))
    if not ok:
        print("== RESULT: PREREQ MISSING")
        return 1

    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    dir_full = os.path.join(WORK, "run-full")
    dir_probe = os.path.join(WORK, "run-probe")
    os.makedirs(dir_full, exist_ok=True)
    os.makedirs(dir_probe, exist_ok=True)

    srv = a_full = a_probe = None
    try:
        with open(C2_LOG, "w", encoding="utf-8", errors="replace") as f:
            srv = subprocess.Popen([C2], stdout=f, stderr=subprocess.STDOUT)
        time.sleep(1)

        devnull = subprocess.DEVNULL
        # 两个模块并发、都用 TCP 长连接（这样 os/plugins 元数据会走 HELLO）
        a_full = subprocess.Popen([FULL, "127.0.0.1", "4444", "-tcp"],
                                  cwd=dir_full, stdout=devnull, stderr=devnull)
        a_probe = subprocess.Popen([PROBE, "127.0.0.1", "4444", "-tcp"],
                                   cwd=dir_probe, stdout=devnull, stderr=devnull)

        t0 = time.time()
        while time.time() - t0 < 25:
            if read(C2_LOG).count(" online from ") >= 2:
                break
            time.sleep(0.5)
        log = read(C2_LOG)

        # 1) 命名空间：各自的 id 文件各归各（EXE 形态落 CWD）
        id_full = os.path.join(dir_full, "full.id")
        id_probe = os.path.join(dir_probe, "probe.id")
        check("full 模块生成 full.id", os.path.exists(id_full))
        check("probe 模块生成 probe.id", os.path.exists(id_probe))
        check("未串名（run-full 下没有 probe.id）",
              not os.path.exists(os.path.join(dir_full, "probe.id")))
        check("未串名（run-probe 下没有 full.id）",
              not os.path.exists(os.path.join(dir_probe, "full.id")))
        h_full, h_probe = hex_of_id_file(id_full), hex_of_id_file(id_probe)
        check("两个模块的 agent_id 不同", h_full and h_probe and h_full != h_probe,
              f"{h_full[:8]}… vs {h_probe[:8]}…")

        # 2) 两个会话都上线（id 与各自 id 文件一致 ⇒ 身份没串）
        check("C2 看到两个模块会话", log.count(" online from ") >= 2,
              f"online={log.count(' online from ')}")
        check("full 会话 id == full.id 内容", h_full and (h_full in log))
        check("probe 会话 id == probe.id 内容", h_probe and (h_probe in log))

        # 3) 模块标签 + 能力位（走 HELLO → 服务端 OSInfo/Plugins）
        check("服务端收到 full 的模块标签",
              'os="Windows (full)"' in log and "plugins=0x00000001" in log)
        check("服务端收到 probe 的模块标签",
              'os="Windows (probe)"' in log and "plugins=0x00000002" in log)

        # 4) 控制端展示（session <id> 打印 os/plugins）
        if h_full:
            p = subprocess.run([RATCTL, "-s", "127.0.0.1:4444"],
                               input=f"session {h_full}\nexit\n",
                               capture_output=True, text=True, timeout=60)
            out = (p.stdout or "") + (p.stderr or "")
            check("ratctl session 显示模块标签", "Windows (full)" in out, repr(out[-90:]))
            check("ratctl session 显示能力位", "0x00000001" in out)
    finally:
        for proc in (a_full, a_probe):
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
