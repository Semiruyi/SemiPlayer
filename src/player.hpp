#pragma once

namespace semi {

// Player 生命周期（lifecycle 层）。init/shutdown 管理整个模块体系的生灭，
// 不属于任何模块。详见 docs/lifecycle.md。
//
// 当前为最小骨架：仅维护 initialized 标志。后续接入：
//   init()    -> IoCContainer::assemble() + ApiLoop::spawn()
//   shutdown() -> ApiLoop stop + IoCContainer::dispose()（逆序释放）
int player_init();
int player_shutdown();

} // namespace semi
