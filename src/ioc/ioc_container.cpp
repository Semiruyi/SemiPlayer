#include "ioc/ioc_container.hpp"

#include "infrastructure/log/log.hpp"

#define SEMI_LOG_TAG "ioc"

namespace semi::ioc {

IoCContainer& IoCContainer::instance() {
    static IoCContainer container;
    return container;
}

bool IoCContainer::assemble() noexcept {
    if (assembled_) {
        SEMI_LOG_INFO("assemble skipped: already assembled");
        return true;
    }

    SEMI_LOG_INFO("assemble begin");
    // 骨架：尚无模块。后续按 DAG 在此 make_shared + 注入依赖；
    // 若某步失败：回滚已构造部分，打日志，return false。
    assembled_ = true;
    SEMI_LOG_INFO("assemble done");
    return true;
}

bool IoCContainer::dispose() noexcept {
    if (!assembled_) {
        SEMI_LOG_INFO("dispose skipped: not assembled");
        return true;
    }

    SEMI_LOG_INFO("dispose begin");
    // 逆序：依赖者先于被依赖者。
    // 后续：api_layer_.reset(); … generation_.reset();
    assembled_ = false;
    SEMI_LOG_INFO("dispose done");
    return true;
}

bool IoCContainer::is_assembled() const noexcept {
    return assembled_;
}

} // namespace semi::ioc
