# check_projects.py — 工程结构自检（不构建，纯静态检查）
#
# 解决的问题：改了名字/搬了文件之后，"工程里还引用着不存在的文件"或"新加的 .cpp 忘了写进工程"
# 都只会表现成 VS 里的怪现象或难查的链接错误。本脚本把这些一次性查出来。
#
# 检查项（对 client/ 下每个 .vcxproj）：
#   A. XML 合法（注意：XML 注释里不能出现 `--`，写分节线很容易踩）
#   B. 引用的每个源/头文件都真实存在
#   C. 每个条目都挂了过滤器，且过滤器都已声明（VS 里就是"分组"）
#   D. 目录里有没有"游离文件"：既没进工程（漏编译）、也不在别人工程里
#
# 用法：python scripts/check_projects.py      （退出码 0=全部通过）
import glob
import os
import sys
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
    out = []
    for p in glob.glob(os.path.join(ROOT, "client", "**", "*.vcxproj"), recursive=True):
        if not p.endswith(".user"):
            out.append(p)
    return sorted(out)


def items_of(root):
    return root.findall(f".//{NS}ClCompile") + root.findall(f".//{NS}ClInclude")


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

        declared = {f.get("Include") for f in root.findall(f".//{NS}Filter") if f.get("Include")}
        missing, unfiltered, undeclared, used = [], [], [], set()
        for it in items_of(root):
            inc = it.get("Include")
            if not inc:
                continue  # ItemDefinitionGroup 里的 <ClCompile>/<Link> 是配置块，不是文件条目
            if not os.path.exists(os.path.join(pdir, inc.replace("\\", os.sep))):
                missing.append(inc)
            f = it.find(f"{NS}Filter")
            text = (f.text or "").strip() if f is not None else ""
            if not text:
                unfiltered.append(inc)
            elif text not in declared:
                undeclared.append(f"{inc} -> {text}")
            else:
                used.add(text)

        check(f"{rel}: 引用的文件都存在", not missing, ", ".join(missing))
        check(f"{rel}: 每个文件都分了组", not unfiltered, ", ".join(unfiltered))
        check(f"{rel}: 分组已声明", not undeclared, ", ".join(undeclared))
        check(f"{rel}: 声明的分组都有文件", declared == used,
              "空分组: " + ", ".join(sorted(declared - used)) if declared != used else "")

    # D. 游离文件：目录里的 .cpp/.h 是否被同目录任一工程引用（按文件名比对）
    dirs = sorted({os.path.dirname(p) for p in vcxprojs()})
    for d in dirs:
        own = [f for f in os.listdir(d) if f.lower().endswith(SRC_EXT)]
        if not own:
            continue
        texts = []
        for p in vcxprojs():
            if os.path.dirname(p) == d:
                texts.append(open(p, encoding="utf-8", errors="replace").read())
        blob = "\n".join(texts)
        orphan = [f for f in own if f not in blob]
        check(f"{os.path.relpath(d, ROOT)}: 无游离源文件", not orphan, ", ".join(orphan))

    print("== RESULT:", "ALL PASS" if ok else "FAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
