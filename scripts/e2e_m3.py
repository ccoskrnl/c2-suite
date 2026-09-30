# e2e_m3.py — Beacon 信道验收（交互式 ratctl）
# 验证：beacon agent 上线、交互式 shell（sessions/use/sysinfo/exec）、
#       命令经 GET 下发、结果经 POST 回传、伪装 HTML、重连同 ID
import subprocess, sys, time, os, threading, queue, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GO = r"C:/Users/202603/.g/go/bin/go.exe"
C2 = os.path.join(ROOT, "bin", "c2server.exe")
RATCTL = os.path.join(ROOT, "bin", "ratctl.exe")
AGENT = os.path.join(ROOT, "client", "x64", "Debug", "RatAgent.exe")
AGENT_DIR = os.path.join(ROOT, "client", "x64", "Debug")
AGENT_ID_FILE = os.path.join(AGENT_DIR, "agent.id")

ok = True
def check(name, cond, detail=""):
    global ok
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
    if not cond: ok = False

class Shell:
    """交互式 ratctl 会话：stdin 发命令，读线程收集 stdout。"""
    def __init__(self):
        self.proc = subprocess.Popen([RATCTL, "-s", "127.0.0.1:4444"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT,
                                     encoding="utf-8", errors="replace", bufsize=1,
                                     cwd=ROOT)
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
    os.makedirs(os.path.join(ROOT, "bin"), exist_ok=True)
    subprocess.run([GO, "build", "-o", C2, "./server/cmd/c2server"], check=True, capture_output=True, cwd=ROOT)
    subprocess.run([GO, "build", "-o", RATCTL, "./ctl/cmd/ratctl"], check=True, capture_output=True, cwd=ROOT)
    if os.path.exists(AGENT_ID_FILE):
        os.remove(AGENT_ID_FILE)

    srv = subprocess.Popen([C2], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    agent = None
    sh = None
    try:
        time.sleep(1)
        agent = subprocess.Popen([AGENT, "127.0.0.1", "8080", "-s", "2"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 cwd=AGENT_DIR)
        time.sleep(4)

        sh = Shell()
        out1 = sh.send(["help", "sessions"], 6)
        check("help 列出命令", "sysinfo" in out1 and "exec" in out1)
        aid = ""
        for line in out1.splitlines():
            parts = line.split()
            if len(parts) >= 1 and len(parts[0]) == 32:
                aid = parts[0]
                break
        check("sessions 列出 beacon agent", len(aid) == 32, aid)

        out2 = sh.send(["use " + aid, "sysinfo"], 8)
        check("use 切换 + 提示符", "ratctl[" in out2 and "switched" in out2)
        check("sysinfo 经轮询回传", "OS:" in out2 and "HOST:" in out2, repr(out2[-100:]))

        out3 = sh.send(["exec echo beacon-m3", "back", "exit"], 8)
        check("exec 经轮询回传", "beacon-m3" in out3, repr(out3[-100:]))
        sh.close(); sh = None

        # GET 响应为伪装 HTML（无命令时无 token div）
        req = urllib.request.Request("http://127.0.0.1:8080/")
        req.add_header("Cookie", "SID=" + aid)
        resp = urllib.request.urlopen(req, timeout=5)
        body = resp.read().decode("utf-8", "replace")
        check("GET 伪装 HTML 且无 token", "Example Domain" in body and 'id="token"' not in body)

        # 重连同 ID
        agent.terminate(); agent.wait(timeout=5)
        time.sleep(1)
        agent = subprocess.Popen([AGENT, "127.0.0.1", "8080", "-s", "2"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 cwd=AGENT_DIR)
        time.sleep(4)
        sh = Shell()
        out4 = sh.send(["sessions", "exit"], 6)
        aid2 = ""
        for line in out4.splitlines():
            parts = line.split()
            if len(parts) >= 1 and len(parts[0]) == 32:
                aid2 = parts[0]
                break
        check("重连后 agent_id 不变", aid2 == aid, f"{aid} -> {aid2}")
        sh.close(); sh = None
    finally:
        if sh: sh.close()
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
