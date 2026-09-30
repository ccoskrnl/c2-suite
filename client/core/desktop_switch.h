// desktop_switch.h — 将调用线程切换到当前接收物理输入的活动桌面
#pragma once

namespace c2 {

// 对应逆向 f_SwitchThreadToInpurDesktop：
//   GetThreadDesktop(GetCurrentThreadId()) 查询当前线程桌面名
//   OpenInputDesktop(0, 0, MAXIMUM_ALLOWED) 打开活动桌面
//   名称不同则 SetThreadDesktop 切换
// 返回是否发生了切换。
bool switch_thread_to_input_desktop();

}  // namespace c2
