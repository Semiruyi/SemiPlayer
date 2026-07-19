#pragma once

#include <functional>
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <utility>

namespace semi::infra {

// 通知中心契约：按通知类型注册回调，按通知类型同步分发通知。
//
// Notifier 不定义任何业务通知类型；业务模块自行定义 struct 事件并通过
// subscribe<T>() / send<T>() 交互。send() 在调用方线程同步执行回调，回调必须轻量，
// 不应阻塞或反向等待发送方。
class Notifier {
public:
    class Subscription {
    public:
        virtual ~Subscription() = default;

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&&) = delete;
        Subscription& operator=(Subscription&&) = delete;

        // true 表示本次调用取消了仍有效的订阅；false 表示该订阅已失效或通知中心已销毁。
        [[nodiscard]] virtual bool unsubscribe() noexcept = 0;
        [[nodiscard]] virtual bool active() const noexcept = 0;

    protected:
        Subscription() = default;
    };

    virtual ~Notifier() = default;

    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;
    Notifier(Notifier&&) = delete;
    Notifier& operator=(Notifier&&) = delete;

    template <class T>
    [[nodiscard]] std::shared_ptr<Subscription> subscribe(std::function<void(const T&)> cb) {
        return subscribe_erased(std::type_index(typeid(T)), [cb = std::move(cb)](const void* event) {
            cb(*static_cast<const T*>(event));
        });
    }

    template <class T>
    // true 表示至少尝试调用了一个仍有效的回调；false 表示该类型当前没有有效回调。
    // 回调抛出的异常由实现记录，不影响本次分发其余回调，也不改变返回值。
    [[nodiscard]] bool send(const T& event) {
        return send_erased(std::type_index(typeid(T)), &event);
    }

    template <class T>
    // true 表示取消了该类型的至少一个订阅；false 表示该类型没有订阅。
    [[nodiscard]] bool clear() noexcept {
        return clear_erased(std::type_index(typeid(T)));
    }

    // true 表示至少取消了一个订阅；false 表示通知中心已为空。
    [[nodiscard]] virtual bool clear_all() noexcept = 0;

protected:
    Notifier() = default;

    virtual std::shared_ptr<Subscription> subscribe_erased(
        std::type_index type,
        std::function<void(const void*)> cb) = 0;

    virtual bool send_erased(std::type_index type, const void* event) = 0;
    virtual bool clear_erased(std::type_index type) noexcept = 0;
};

} // namespace semi::infra
