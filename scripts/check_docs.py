# check_docs.py — 文档自检（不构建、不联网）
#
# 解决的问题：文档里的**本地链接**（指向源码/脚本/其它文档）在改名、搬家、删除之后会变成死链，
# 而这一点在浏览器里不一定立刻暴露。改名/搬文件/加文件之后跑一下即可。
#
# 检查项：
#   A. README.md 与 docs/*.md 里所有**本地** Markdown 链接指向的文件/目录是否存在
#      （http(s)/锚点/邮件链接跳过）
#   B. 文档里以反引号引用的 `scripts/xxx.py`、`docs/xxx.md`、`client/...` 形式路径是否存在
#
# 用法：python scripts/check_docs.py      （退出码 0=全部通过）
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = ["README.md"] + [os.path.relpath(p, ROOT) for p in sorted(glob.glob(os.path.join(ROOT, "docs", "*.md")))]

# 整份跳过的文档：原始设计计划书，其目录结构**未按此实现**（历史/设想），不参与检查
IGNORE_DOCS = {"docs/ARCHITECTURE.md"}

# 生成目录 / 仓库本就不托管的第三方依赖：文档里出现是正常的，不要求存在
IGNORE_PREFIXES = (
    "build/", "bin/",                     # 构建输出
    "im_ctl/build/", "viewer/build/",     # CMake 构建目录
    "im_ctl/external/",                   # 第三方依赖（.gitignore 明确不托管）
    "client/x64/", "client/RatAgent/x64/",  # VS 默认输出/中间目录
)

# 历史名：改名/搬家后**在"旧 → 新"对照记录里刻意保留**，或已被取代；每条都写明理由
IGNORE_TICKS = {
    "client/RatAgent.vcxproj": "§9.7 改名对照表里的旧名（现 RatAgent.sys.exe.vcxproj）",
    "client/RatAgentDll.vcxproj": "§9.7 改名对照表里的旧名（现 RatAgent.sys.vcxproj）",
    "client/RatAgent.sln": "§9.8 改名对照表里的旧名（现 agents.sln）",
    "client/caps_full.cpp": "§9.5/§9.7 改名对照表里的旧名（现 caps_sys.cpp）",
    "scripts/e2e_caps.py": "已被 e2e_screen.py 取代（§9.3 历史记录）",
    "im_ctl/ctl_client.h/.cpp": "简写：指 im_ctl/ctl_client.h 与 im_ctl/ctl_client.cpp 两个文件",
}

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
# 反引号里的相对路径（只认我们仓库里真实存在的那些前缀，避免把命令/示例误判）
TICK_RE = re.compile(r"`((?:scripts|docs|client|server|ctl|tools|viewer|im_ctl|internal|cmd)/[\w./\\-]+)`")

ok = True


def check(name, cond, detail=""):
    global ok
    if not cond:
        ok = False
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  {detail}" if detail else ""))


def main():
    links = bad_links = ticks = bad_ticks = 0

    for doc in DOCS:
        if doc.replace("\\", "/") in IGNORE_DOCS:
            continue
        path = os.path.join(ROOT, doc)
        base = os.path.dirname(path)
        text = open(path, encoding="utf-8", errors="replace").read()

        for m in LINK_RE.finditer(text):
            target = m.group(1).strip()
            if target.startswith(("http://", "https://", "#", "mailto:")):
                continue
            rel = target.split("#")[0]
            if not rel or rel.replace("\\", "/").startswith(IGNORE_PREFIXES):
                continue
            links += 1
            if not os.path.exists(os.path.normpath(os.path.join(base, rel))):
                bad_links += 1
                print(f"      死链: {doc} -> {target}")

        for m in TICK_RE.finditer(text):
            rel = m.group(1)
            if rel in IGNORE_TICKS or rel.replace("\\", "/").startswith(IGNORE_PREFIXES):
                continue
            ticks += 1
            if not os.path.exists(os.path.join(ROOT, rel.replace("\\", os.sep))):
                bad_ticks += 1
                print(f"      反引号路径不存在: {doc} -> {rel}")

    check(f"本地 Markdown 链接有效（{len(DOCS)} 个文档，{links} 条）", bad_links == 0,
          f"{bad_links} 条失效")
    check(f"反引号引用的仓库路径存在（{ticks} 处）", bad_ticks == 0, f"{bad_ticks} 处失效")

    print("== RESULT:", "ALL PASS" if ok else "FAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
