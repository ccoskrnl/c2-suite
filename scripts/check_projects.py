# check_projects.py — 工程结构自检（不构建，纯静态检查）
#
# 解决的问题：改名/搬文件/加文件之后，"工程里还引用着不存在的文件"、"新加的 .cpp 忘了写进工程"、
# ".filters 与 .vcxproj 不同步（VS 里分组就乱了）" 这些都只会表现成 VS 里的怪现象或难查的编译错误。
#
# 检查项（对 client/ 下每个 .vcxproj）：
#   A. .vcxproj 的 XML 合法（XML 注释里不能出现 `--`，写分节线很容易踩）
#   B. 引用的每个源/头文件都真实存在
#   C. <工程名>.vcxproj.filters 存在且 XML 合法
#      —— VS 的 C++ 工程**只读这个文件**来显示分组；写在 .vcxproj 里的 <Filter> 无效
#   D. 工程里每个文件都在 .filters 里挂了分组，分组都已声明；反过来 .filters 里没有工程里不存在的幽灵项
#   E. 目录里有没有"游离源文件"：写了 .cpp/.h 却没进工程（会漏编译）
#
# 用法：python scripts/check_projects.py      （退出码 0=全部通过）
import glob
import os
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NS = "{http://schemas.microsoft.com/developer/msbuild/2003}"
SRC_EXT = (".cpp", ".h")

ok = True


def check(name, cond, detail=""):
    global ok
    if not cond:
        ok = False
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  {detail}" if detail else ""))


def vcxprojs():
    out = [p for p in glob.glob(os.path.join(ROOT, "client", "**", "*.vcxproj"), recursive=True)
           if not p.endswith(".user")]
    return sorted(out)


def file_items(root):
    """工程的源/头文件条目（排除 ItemDefinitionGroup 里的配置块，它们没有 Include）。"""
    out = []
    for tag in ("ClCompile", "ClInclude"):
        for it in root.findall(f".//{NS}{tag}"):
            if it.get("Include"):
                out.append((tag, it.get("Include")))
    return out


def main():
    for proj in vcxprojs():
        rel = os.path.relpath(proj, ROOT)
        pdir = os.path.dirname(proj)
        try:
            root = ET.parse(proj).getroot()
        except ET.ParseError as e:
            check(f"{rel}: XML 合法", False, str(e))
            continue
        check(f"{rel}: XML 合法", True)

        items = file_items(root)
        missing = [inc for _, inc in items
                   if not os.path.exists(os.path.join(pdir, inc.replace("\\", os.sep)))]
        check(f"{rel}: 引用的文件都存在", not missing, ", ".join(missing))

        # ---- .filters（VS 的分组来源）----
        fpath = proj + ".filters"
        if not os.path.exists(fpath):
            check(f"{rel}: 有 .vcxproj.filters", False, "缺失 → VS 里会平铺显示")
            continue
        check(f"{rel}: 有 .vcxproj.filters", True)
        try:
            froot = ET.parse(fpath).getroot()
        except ET.ParseError as e:
            check(f"{rel}.filters: XML 合法", False, str(e))
            continue
        check(f"{rel}.filters: XML 合法", True)

        declared = {f.get("Include") for f in froot.findall(f".//{NS}Filter") if f.get("Include")}
        assigned = {}
        for tag in ("ClCompile", "ClInclude"):
            for it in froot.findall(f".//{NS}{tag}"):
                inc = it.get("Include")
                if not inc:
                    continue
                fl = it.find(f"{NS}Filter")
                assigned[inc] = (fl.text or "").strip() if fl is not None else ""

        proj_files = {inc for _, inc in items}
        unassigned = sorted(proj_files - set(assigned))
        ghosts = sorted(set(assigned) - proj_files)
        undeclared = sorted(f"{i} -> {f}" for i, f in assigned.items() if f and f not in declared)
        used = {f for f in assigned.values() if f}
        empty = sorted(declared - used)

        check(f"{rel}.filters: 工程里每个文件都挂了分组", not unassigned, ", ".join(unassigned))
        check(f"{rel}.filters: 分组都已声明", not undeclared, ", ".join(undeclared))
        check(f"{rel}.filters: 无幽灵项（工程里没有的文件）", not ghosts, ", ".join(ghosts))
        check(f"{rel}.filters: 声明的分组都有文件", not empty, ", ".join(empty))

    # E. 游离文件：目录里的 .cpp/.h 是否被同目录任一工程引用（按文件名比对）
    for d in sorted({os.path.dirname(p) for p in vcxprojs()}):
        own = [f for f in os.listdir(d) if f.lower().endswith(SRC_EXT)]
        if not own:
            continue
        blob = "\n".join(open(p, encoding="utf-8", errors="replace").read()
                         for p in vcxprojs() if os.path.dirname(p) == d)
        orphan = [f for f in own if f not in blob]
        check(f"{os.path.relpath(d, ROOT)}: 无游离源文件", not orphan, ", ".join(orphan))

    print("== RESULT:", "ALL PASS" if ok else "FAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
