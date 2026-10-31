#pragma once

#include "medicon/gui/GuiContext.hpp"
#include "medicon/gui/GuiMath.hpp"

#include <array>

namespace medicon::gui {

// How the glass counter moves, apart from drawing so it can be tested directly.
class BlockCountAnimator {
public:
    static constexpr std::size_t digit_capacity = 4;
    struct Digit {
        int value = -1;    // -1 when this place is empty
        int previous = -1; // what it is rolling away from
        float progress = 1.0F;
    };
    struct Metrics {
        float digit_width = 0.0F;
        float padding = 0.0F;
        float height = 0.0F;
    };

    // A negative count dissolves the glass; it keeps its last digits meanwhile.
    void update(int count, const Metrics& metrics, float sway, float dt);

    [[nodiscard]] bool visible() const noexcept { return shown_; }
    [[nodiscard]] float width() const noexcept { return width_.value; }
    [[nodiscard]] float height(const Metrics& metrics) const noexcept;
    [[nodiscard]] float waist() const noexcept;
    [[nodiscard]] float energy() const noexcept;
    [[nodiscard]] float light() const noexcept { return light_; }
    // Glass does not fade in. It rises at full size and gains its optics.
    [[nodiscard]] float lens() const noexcept;
    [[nodiscard]] float opacity() const noexcept;
    [[nodiscard]] float scale() const noexcept;
    // Fully formed and no longer moving: what it draws can land on whole pixels.
    [[nodiscard]] bool at_rest() const noexcept;
    // 1 when the count last rose, -1 when it fell.
    [[nodiscard]] float direction() const noexcept { return direction_; }
    // Place 0 is the units.
    [[nodiscard]] const Digit& digit(std::size_t place) const noexcept { return digits_[place]; }

private:
    void set_count(int count);

    std::array<Digit, digit_capacity> digits_{};
    Spring width_, squeeze_, press_, appear_;
    int count_ = -1;
    int places_ = 1;
    float light_ = 0.0F;
    float direction_ = -1.0F;
    bool shown_ = false;
};

// The blocks Scaffold has left, on a pane of liquid glass below the crosshair.
class BlockCount {
public:
    void render(GuiContext& context);

private:
    BlockCountAnimator animator_;
};

} // namespace medicon::gui
