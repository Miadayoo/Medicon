#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace medicon {

// Single-threaded event dispatcher. Keep each returned token for as long as the
// subscription should remain active. A token may safely outlive the bus.
class EventBus {
    struct Slot {
        bool active = true;
        std::function<void(void*)> callback;
    };

public:
    class Subscription {
    public:
        Subscription() = default;
        ~Subscription() { reset(); }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept : slot_(std::move(other.slot_)) {
            other.slot_.reset();
        }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                slot_ = std::move(other.slot_);
                other.slot_.reset();
            }
            return *this;
        }
        void reset() noexcept {
            if (auto slot = slot_.lock()) slot->active = false;
            slot_.reset();
        }

    private:
        friend class EventBus;
        explicit Subscription(const std::shared_ptr<Slot>& slot) : slot_(slot) {}
        std::weak_ptr<Slot> slot_;
    };

    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    template <class Event, class Callback>
    Subscription subscribe(Callback&& callback) {
        static_assert(!std::is_const_v<Event>, "Use a non-const event type");
        auto& slots = slots_[std::type_index(typeid(Event))];
        std::erase_if(slots, [](const auto& slot) { return !slot->active; });
        auto slot = std::make_shared<Slot>();
        slot->callback = [handler = std::forward<Callback>(callback)](void* event) mutable {
            std::invoke(handler, *static_cast<Event*>(event));
        };
        slots.push_back(slot);
        return Subscription{slot};
    }

    template <class Event> void publish(Event& event) {
        static_assert(!std::is_const_v<Event>, "Publish a mutable event reference");
        const auto found = slots_.find(std::type_index(typeid(Event)));
        if (found == slots_.end()) return;
        // Snapshot excludes newly added listeners from this dispatch. Shared
        // slots remain valid through nested publish, subscribe and disconnect.
        const auto snapshot = found->second;
        for (const auto& slot : snapshot) {
            if (slot->active) slot->callback(&event);
        }
    }

private:
    std::unordered_map<std::type_index, std::vector<std::shared_ptr<Slot>>> slots_;
};

} // namespace medicon
