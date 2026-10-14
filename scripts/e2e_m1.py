# e2e_m1.py — M1 联调验收：c2server + 真实 C++ Agent + ratctl list
# 1) C++ Agent 上线、list 可见、online=yes（真实握手/心跳）
# 2) Python 哑 Agent：握手后保持连接但不发心跳 → 30s 标记 offline（连接仍在）、60s 回收
import subprocess, sys, time, os, socket, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GO = r"C:/Users/202603/.g/go/bin/go.exe"
C2 = os.path.join(ROOT, "bin", "c2server.exe")
RATCTL = os.path.join(ROOT, "bin", "ratctl.exe")
AGENT = os.path.join(ROOT, "client", "x64", "Debug", "RatAgent.sys.exe")
CPP_AGENT_ID = None  # C++ Agent 上线后动态获取（agent_id 现为随机持久化）
PY_AGENT_ID  = "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"   # 哑 Agent id

ok = True
def check(name, cond, detail=""):
    global ok
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
    if not cond: ok = False

def build():
    os.makedirs(os.path.join(ROOT, "bin"), exist_ok=True)
    subprocess.run([GO, "build", "-o", C2, "./server/cmd/c2server"], check=True,
                   capture_output=True, cwd=ROOT)
    subprocess.run([GO, "build", "-o", RATCTL, "./ctl/cmd/ratctl"], check=True,
                   capture_output=True, cwd=ROOT)

def list_sessions():
    r = subprocess.run([RATCTL, "-s", "127.0.0.1:4444", "list"],
                       capture_output=True, text=True, timeout=10)
    if r.returncode != 0:
        return []
    return [ln for ln in r.stdout.splitlines()[1:] if ln.strip()]

def find(row_prefix, rows):
    return [r for r in rows if r.startswith(row_prefix)]

def silent_agent(agent_id_hex):
    """握手后保持连接、不发任何包（模拟静默/网络故障半开连接）。"""
    s = socket.create_connection(("127.0.0.1", 4444), timeout=5)
    agent_id = bytes.fromhex(agent_id_hex)
    client_nonce = bytes(range(1, 17))
    payload = struct.pack("<IB", 0x00010001, 0) + agent_id + client_nonce \
            + struct.pack("<IH", 0, len(b"Windows")) + b"Windows"
    hdr = struct.pack("<HBBIIHBBI", 0x5A5A, 1, 0, 1, 0, 0x0101, 0, 0, len(payload))
    s.sendall(hdr + payload)
    ack_hdr = recv_exact(s, 20)
    ack_len = struct.unpack("<I", ack_hdr[16:20])[0]
    recv_exact(s, ack_len)
    return s

def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise ConnectionError("closed")
        b += c
    return b

def main():
    build()
    srv = subprocess.Popen([C2], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.5)

        # ---- 1) C++ Agent 上线 ----
        agent = subprocess.Popen([AGENT, "127.0.0.1", "4444"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2.5)
        rows = list_sessions()
        # agent_id 为随机生成，动态取 list 第一行
        if rows:
            cpp_id = rows[0].split()[0]
            global CPP_AGENT_ID
            CPP_AGENT_ID = cpp_id
        hit = find(CPP_AGENT_ID, rows)
        check("C++ Agent 上线且 list 可见", len(hit) == 1, f"rows={len(rows)} id={CPP_AGENT_ID}")
        check("C++ Agent online=yes", hit and hit[0].strip().endswith("yes"))

        # ---- 2) 哑 Agent 静默测试 ----
        silent = silent_agent(PY_AGENT_ID)
        time.sleep(2)
        rows = list_sessions()
        hit = find(PY_AGENT_ID, rows)
        check("哑 Agent 握手后上线", len(hit) == 1)

        print("    (哑 Agent 保持连接但不发心跳，等待 32s...)")
        time.sleep(32)  # >30s 离线判定
        rows = list_sessions()
        hit = find(PY_AGENT_ID, rows)
        check("30s 后哑 Agent 标记离线(online=no)", hit and hit[0].strip().endswith("no"),
              repr(hit[0] if hit else ""))
        check("离线时连接仍保留(未被立即回收)", len(hit) == 1)

        print("    (再等 32s，累计 >60s，应回收并关闭连接...)")
        time.sleep(32)
        rows = list_sessions()
        check("60s 后会话被回收", len(find(PY_AGENT_ID, rows)) == 0)
        # server 应主动关闭该连接：recv 返回 0/异常
        try:
            silent.settimeout(3)
            data = silent.recv(1)
            closed = data == b""
        except OSError:
            closed = True
        check("server 主动关闭离线连接", closed)
        silent.close()

        # 清理 C++ agent
        agent.terminate()
        try: agent.wait(timeout=5)
        except subprocess.TimeoutExpired: agent.kill()
    finally:
        srv.terminate()
        try: srv.wait(timeout=3)
        except subprocess.TimeoutExpired: srv.kill()

    print("== RESULT:", "ALL PASS" if ok else "FAILURES PRESENT")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
