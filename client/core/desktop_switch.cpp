// desktop_switch.cpp — 实现见头文件；逻辑对照逆向 f_SwitchThreadToInpurDesktop
#include "desktop_switch.h"

#include <windows.h>

namespace c2 {

bool switch_thread_to_input_desktop() {
    DWORD tid = GetCurrentThreadId();
    HDESK current = GetThreadDesktop(tid);  // 当前线程关联的桌面（无需 Close）
    if (!current)
        return false;

    WCHAR current_name[256] = {0};
    DWORD needed = 0;
    GetUserObjectInformationW(current, UOI_NAME, current_name,
                              sizeof(current_name), &needed);

    // 打开当前接收物理输入的桌面（活动桌面），权限 MAXIMUM_ALLOWED
    HDESK input = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
    if (!input)
        return false;

    WCHAR input_name[256] = {0};
    GetUserObjectInformationW(input, UOI_NAME, input_name,
                              sizeof(input_name), &needed);

    bool switched = false;
    if (lstrcmpiW(current_name, input_name) != 0) {
        // 桌面不同：把当前线程切换到活动桌面，之后 GetDC(0)/注入才作用到用户看到的桌面
        switched = SetThreadDesktop(input) != 0;
    }

    CloseDesktop(input);
    return switched;
}

}  // namespace c2
