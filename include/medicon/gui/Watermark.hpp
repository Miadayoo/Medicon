#pragma once

#include "medicon/gui/GuiContext.hpp"
#include "medicon/gui/GuiMath.hpp"

#include <string>
#include <string_view>

namespace medicon::gui {

// "Medicon" on a Main Menu glass plate at the top left, drawn only while
// playing (spec: Watermark). Fades, rises 5 px and grows from 0.97 when it
// appears; the same in reverse when the module is turned off. Japanese mode
// replaces the plate with めじこん in the inject title's style, with the same motion.
class Watermark {
public:
    Watermark();
    // `wall_dt`: unclamped seconds since the last frame, for the FPS reading.
    void render(GuiContext& context, float wall_dt);
    // Not drawn this frame (Main Menu, Social covering the game): the next
    // appearance plays the entrance again.
    void hide() noexcept;
    // Shown or fading: its font sizes must stay in the atlas.
    bool visible() const noexcept { return fade_.p > 0.0F; }

    // "v1.0.5": the newest release on the update server when the loader
    // reached it, otherwise this build's version.
    const std::string& version() const noexcept { return version_; }
    // `text` without the leading "v"; empty keeps the build's version.
    void set_version(std::string_view text);

private:
    Reversible fade_;
    float width_ = 0.0F;   // drawn plate width, px; follows the measured width
    float scale_ = 0.0F;   // layout scale of width_; a change snaps the width
    float fps_time_ = 0.0F;
    int fps_frames_ = 0;
    int fps_ = 0;          // last reading, 0 until the first window closes
    std::string fps_text_;
    std::string version_;
};

} // namespace medicon::gui
