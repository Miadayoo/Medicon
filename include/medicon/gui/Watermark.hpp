#pragma once

#include "medicon/gui/GuiContext.hpp"
#include "medicon/gui/GuiMath.hpp"

#include <string>

namespace medicon::gui {

// "Medicon" on a Main Menu glass plate at the top left, drawn only while
// playing (spec: Watermark). Fades, rises 5 px and grows from 0.97 when it
// appears; the same in reverse when the module is turned off.
class Watermark {
public:
    // `wall_dt`: unclamped seconds since the last frame, for the FPS reading.
    void render(GuiContext& context, float wall_dt);
    // Not drawn this frame (Main Menu, Social covering the game): the next
    // appearance plays the entrance again.
    void hide() noexcept;
    // Shown or fading: its font sizes must stay in the atlas.
    bool visible() const noexcept { return fade_.p > 0.0F; }

    // "v0.1.0".
    static const char* version() noexcept;

private:
    Reversible fade_;
    float width_ = 0.0F;   // drawn plate width, px; follows the measured width
    float scale_ = 0.0F;   // layout scale of width_; a change snaps the width
    float fps_time_ = 0.0F;
    int fps_frames_ = 0;
    int fps_ = 0;          // last reading, 0 until the first window closes
    std::string fps_text_;
};

} // namespace medicon::gui
