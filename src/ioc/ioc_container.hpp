#pragma once

#include <memory>

namespace semi::application {
class ApiLayer;
}

namespace semi::ioc {

// 模块体系装配器（见 docs/modules/ioc_container/ioc_container.md）。
//
// 进程内单例：与 SemiPlayer「全局唯一播放器」模型一致。
//   instance()  — 取壳（不触发装配）
//   assemble()  — 按 DAG 构造模块、注入依赖；bool 成功/失败（幂等成功）
//   dispose()   — 逆序释放；bool 成功/失败（幂等成功）
//
// 结果约定见 docs/error_convention.md：内部用 bool，C ABI 再映射 semi_status。
// 当前装配 ApiLayer；后续按 DAG 扩展业务模块并注入其依赖。
// 线程约定：assemble / dispose 为单线程控制面操作。
class IoCContainer {
public:
    static IoCContainer& instance();

    IoCContainer(const IoCContainer&) = delete;
    IoCContainer& operator=(const IoCContainer&) = delete;
    IoCContainer(IoCContainer&&) = delete;
    IoCContainer& operator=(IoCContainer&&) = delete;

    /// 装配当前已注册模块。已装配 → true（幂等）。失败 → false。
    [[nodiscard]] bool assemble() noexcept;

    /// 逆序释放。未装配 → true（幂等）。失败 → false。
    [[nodiscard]] bool dispose() noexcept;

    [[nodiscard]] bool is_assembled() const noexcept;

    // 仅供进程边界（api_export）在已装配后获取 ApiLayer。业务模块仍必须使用
    // 构造期注入，禁止借 IoC 做运行时服务定位。
    [[nodiscard]] std::shared_ptr<application::ApiLayer> api_layer() const noexcept;

private:
    IoCContainer() = default;
    ~IoCContainer() = default;

    bool assembled_ = false;
    std::shared_ptr<application::ApiLayer> api_layer_;
    // 后续：按装配顺序声明 std::shared_ptr<Module>；dispose 手动逆序 reset。
};

} // namespace semi::ioc
