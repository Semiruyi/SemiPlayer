#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <tuple>
#include <type_traits>

namespace semi::infra {

// A sink invoked synchronously by RealTimeNotifier::notify(). Implementations
// must be noexcept and satisfy the publisher's real-time constraints.
template <class Event>
class RealTimeNotificationSink {
public:
    virtual ~RealTimeNotificationSink() = default;

    RealTimeNotificationSink(const RealTimeNotificationSink&) = delete;
    RealTimeNotificationSink& operator=(const RealTimeNotificationSink&) = delete;
    RealTimeNotificationSink(RealTimeNotificationSink&&) = delete;
    RealTimeNotificationSink& operator=(RealTimeNotificationSink&&) = delete;

    virtual void on_realtime_notification(const Event& event) noexcept = 0;

protected:
    RealTimeNotificationSink() = default;
};

// Declares one compile-time event route and its fixed subscriber capacity.
template <class Event, std::size_t MaxSinks>
struct RealTimeEventSpec {
    static_assert(MaxSinks > 0, "a real-time event route needs at least one sink slot");

    using event_type = Event;
    static constexpr std::size_t max_sinks = MaxSinks;
};

namespace detail {

template <class Event, class... Specs>
inline constexpr bool has_realtime_event_v =
    (std::is_same_v<Event, typename Specs::event_type> || ...);

template <class Event, class... Specs>
struct RealtimeEventSpec;

template <class Event, std::size_t MaxSinks, class... Rest>
struct RealtimeEventSpec<Event, RealTimeEventSpec<Event, MaxSinks>, Rest...> {
    using type = RealTimeEventSpec<Event, MaxSinks>;
};

template <class Event, class First, class... Rest>
struct RealtimeEventSpec<Event, First, Rest...>
    : RealtimeEventSpec<Event, Rest...> {
};

template <class Event>
struct RealtimeEventSpec<Event>;

template <class Event, std::size_t MaxSinks>
class RealTimeChannel {
public:
    using Sink = RealTimeNotificationSink<Event>;

    [[nodiscard]] bool register_sink(Sink& sink) noexcept {
        for (std::size_t index = 0; index < sink_count_; ++index) {
            if (sinks_[index] == &sink) {
                return false;
            }
        }
        if (sink_count_ == MaxSinks) {
            return false;
        }
        sinks_[sink_count_++] = &sink;
        return true;
    }

    [[nodiscard]] bool unregister_sink(Sink& sink) noexcept {
        for (std::size_t index = 0; index < sink_count_; ++index) {
            if (sinks_[index] != &sink) {
                continue;
            }

            for (std::size_t next = index + 1; next < sink_count_; ++next) {
                sinks_[next - 1] = sinks_[next];
            }
            sinks_[--sink_count_] = nullptr;
            return true;
        }
        return false;
    }

    void notify(const Event& event) const noexcept {
        for (std::size_t index = 0; index < sink_count_; ++index) {
            sinks_[index]->on_realtime_notification(event);
        }
    }

private:
    std::array<Sink*, MaxSinks> sinks_{};
    std::size_t sink_count_ = 0;
};

} // namespace detail

// A synchronous, allocation-free notification router for real-time publishers.
//
// Event routes and their capacities are declared in the type. Callers may register
// or unregister sinks only while the notifier is unsealed. Before the publisher
// thread starts, call seal(). Stop and join the publisher thread before unseal().
//
// No public operation on one instance is thread-safe. The caller must serialize
// every call, including notify(): one notifier has one publisher thread. Use
// separate notifier instances for multiple real-time publishers. Violating this
// lifecycle is a caller error; this type intentionally provides no concurrent
// registration, dispatch, or lifetime management.
template <class... Specs>
class RealTimeNotifier {
    static_assert(sizeof...(Specs) > 0, "a real-time notifier needs at least one event route");

public:
    RealTimeNotifier() = default;

    RealTimeNotifier(const RealTimeNotifier&) = delete;
    RealTimeNotifier& operator=(const RealTimeNotifier&) = delete;
    RealTimeNotifier(RealTimeNotifier&&) = delete;
    RealTimeNotifier& operator=(RealTimeNotifier&&) = delete;

    template <class Event>
        requires detail::has_realtime_event_v<Event, Specs...>
    // Control-plane only. Must not run concurrently with any notifier operation.
    [[nodiscard]] bool register_sink(RealTimeNotificationSink<Event>& sink) noexcept {
        assert(!sealed_);
        if (sealed_) {
            return false;
        }
        return channel<Event>().register_sink(sink);
    }

    template <class Event>
        requires detail::has_realtime_event_v<Event, Specs...>
    // Control-plane only. Must not run concurrently with any notifier operation.
    [[nodiscard]] bool unregister_sink(RealTimeNotificationSink<Event>& sink) noexcept {
        assert(!sealed_);
        if (sealed_) {
            return false;
        }
        return channel<Event>().unregister_sink(sink);
    }

    // Control-plane only. Must not run concurrently with any notifier operation.
    [[nodiscard]] bool seal() noexcept {
        if (sealed_) {
            return false;
        }
        sealed_ = true;
        return true;
    }

    // Control-plane only. The publisher thread must already be stopped and joined.
    [[nodiscard]] bool unseal() noexcept {
        assert(sealed_);
        if (!sealed_) {
            return false;
        }
        sealed_ = false;
        return true;
    }

    template <class Event>
        requires detail::has_realtime_event_v<Event, Specs...>
    // Publisher-plane only. Exactly one publisher thread may call notify().
    void notify(const Event& event) const noexcept {
        assert(sealed_);
        if (!sealed_) {
            return;
        }
        channel<Event>().notify(event);
    }

    [[nodiscard]] bool sealed() const noexcept { return sealed_; }

private:
    template <class Event>
    using EventSpec = typename detail::RealtimeEventSpec<Event, Specs...>::type;

    template <class Event>
    [[nodiscard]] detail::RealTimeChannel<Event, EventSpec<Event>::max_sinks>& channel() noexcept {
        return std::get<detail::RealTimeChannel<Event, EventSpec<Event>::max_sinks>>(channels_);
    }

    template <class Event>
    [[nodiscard]] const detail::RealTimeChannel<Event, EventSpec<Event>::max_sinks>& channel() const noexcept {
        return std::get<detail::RealTimeChannel<Event, EventSpec<Event>::max_sinks>>(channels_);
    }

    std::tuple<detail::RealTimeChannel<typename Specs::event_type, Specs::max_sinks>...> channels_;
    bool sealed_ = false;
};

} // namespace semi::infra
