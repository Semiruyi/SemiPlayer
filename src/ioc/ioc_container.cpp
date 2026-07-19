#include "ioc/ioc_container.hpp"

#include "application/api_layer.hpp"
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
    try {
        auto api_layer = std::make_shared<application::ApiLayer>();
        if (!api_layer->start()) {
            SEMI_LOG_ERROR("ApiLayer start failed");
            return false;
        }
        api_layer_ = std::move(api_layer);
    } catch (...) {
        SEMI_LOG_ERROR("ApiLayer assemble failed");
        return false;
    }
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
    // 逆序：依赖者先于被依赖者。ApiLayer 必须先排空其命令线程。
    if (api_layer_ && !api_layer_->stop()) {
        SEMI_LOG_ERROR("ApiLayer stop failed");
        return false;
    }
    api_layer_.reset();
    assembled_ = false;
    SEMI_LOG_INFO("dispose done");
    return true;
}

bool IoCContainer::is_assembled() const noexcept {
    return assembled_;
}

std::shared_ptr<application::ApiLayer> IoCContainer::api_layer() const noexcept {
    return api_layer_;
}

} // namespace semi::ioc
