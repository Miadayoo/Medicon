#include <medicon/platform/MouseCaptureController.hpp>
#include <medicon/platform/RawMouseWheel.hpp>
#include <medicon/platform/RelativeMousePointer.hpp>
#include <medicon/platform/OverlayInput.hpp>
#ifdef _WIN32
#include <medicon/platform/MenuInputValidation.hpp>
#endif

#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>

namespace {
using medicon::platform::MouseCaptureController;
using medicon::platform::MouseSyncSchedule;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Backend {
    std::optional<bool> grabbed = true;
    unsigned releases = 0;
    unsigned grabs = 0;
    bool release_succeeds = true;
    bool grab_succeeds = true;

    bool release() {
        ++releases;
        if (release_succeeds) grabbed = false;
        return release_succeeds;
    }
    bool grab() {
        ++grabs;
        if (grab_succeeds) grabbed = true;
        return grab_succeeds;
    }
    bool update(MouseCaptureController& controller, bool open, bool focused = true) {
        return controller.update(open, focused, grabbed, [this] { return release(); }, [this] { return grab(); });
    }
    bool reset(MouseCaptureController& controller, bool focused) {
        return controller.reset(focused, [this] { return grab(); });
    }
};

void opening_and_reassertion() {
    MouseCaptureController controller;
    Backend backend;
    check(!controller.active(), "Fresh controller unexpectedly required native polling");
    check(backend.update(controller, false), "Closed initial state failed");
    check(backend.releases == 0 && backend.grabs == 0, "Closed initial state changed native capture");
    check(backend.update(controller, true), "Opening did not release capture");
    check(backend.releases == 1 && backend.grabbed == false, "Opening did not invoke native release exactly once");
    check(controller.active(), "Open GUI did not retain capture management");
    check(backend.update(controller, true) && backend.releases == 1, "Already-free open state repeatedly released capture");
    backend.grabbed = true; // A native game transition tries to capture again.
    check(backend.update(controller, true) && backend.releases == 2 && backend.grabbed == false,
          "A repeated native grab was not released while the GUI stayed open");
    check(backend.update(controller, false) && backend.grabs == 1 && backend.grabbed == true,
          "Closing did not restore the original native grab");
    check(!controller.active(), "Completed restoration did not return to an idle state");
    check(backend.update(controller, false) && backend.grabs == 1, "Stable closed state grabbed again");
}

void overlay_press_keeps_its_owner_until_release() {
    medicon::platform::InputPressOwnership<256> input;
    check(input.consume(1, true, true), "Social mouse down leaked to the game");
    check(input.consume(1, true, false), "Closing Social leaked a held mouse repeat");
    check(input.consume(1, false, false), "Closing Social leaked the matching mouse up");
    check(!input.active(), "Released overlay press retained its ownership");
    check(!input.consume(1, true, false) && !input.consume(1, false, false),
          "A new game click was swallowed after the overlay press ended");

    check(!input.consume('W', true, false), "ClickGUI swallowed a movement press");
    check(input.consume('W', true, true), "A modal screen leaked a held movement repeat");
    check(!input.consume('W', false, true), "Opening a modal screen left a game movement key stuck");
    check(input.consume('W', true, true), "Modal text input leaked a new movement press");
    check(input.consume('W', false, false), "Closing text input leaked a key release");
    check(!input.consume('W', true, false), "Movement did not resume on a new press");
    input.reset();
    check(!input.active() && !input.consume('W', false, false), "Focus reset retained press ownership");

    for (const int key : std::array<int, 11>{'W', 'A', 'S', 'D', 0x20, 0x10, 0xA0, 0xA1, 0x11, 0xA2, 0xA3})
        check(medicon::platform::clickgui_movement_key(key), "A default movement key was not allowed in ClickGUI");
    for (const int key : std::array<int, 10>{1, 2, 4, 5, 6, 'E', 'T', 0x1B, 0x24, 0x2D})
        check(!medicon::platform::clickgui_movement_key(key), "A mouse/menu/chat key was allowed as movement");
}

void preserve_already_free_inventory() {
    MouseCaptureController controller;
    Backend backend;
    backend.grabbed = false; // Inventory or another native screen already owns the cursor.
    check(backend.update(controller, true), "Opening over an already-free screen failed");
    check(backend.releases == 0, "Already-free inventory state was redundantly released");
    backend.grabbed = true;
    check(backend.update(controller, true) && backend.releases == 1, "Unexpected native grab was not released");
    check(backend.update(controller, false) && backend.grabs == 0,
          "Closing over an initially free screen unexpectedly grabbed the mouse");
}

void unknown_state_is_conservative() {
    MouseCaptureController controller;
    unsigned releases = 0;
    unsigned grabs = 0;
    const auto release = [&] { ++releases; return true; };
    const auto grab = [&] { ++grabs; return true; };
    check(controller.update(true, true, std::nullopt, release, grab) && releases == 1,
          "Unknown initial state did not attempt release");
    check(controller.update(true, true, std::nullopt, release, grab) && releases == 1,
          "Unknown state repeatedly released despite an earlier successful release");
    check(controller.update(false, true, std::nullopt, release, grab) && grabs == 0,
          "Unknown original state unexpectedly forced a grab on close");
    check(controller.update(true, true, std::nullopt, release, grab) && releases == 2,
          "A new unknown open transition did not release again");
    check(controller.reset(true, grab) && grabs == 0, "Unknown original state unexpectedly grabbed during reset");
}

void focus_defers_restoration() {
    MouseCaptureController controller;
    Backend backend;
    check(backend.update(controller, true), "Opening before focus change failed");
    check(backend.update(controller, true, false), "Open focus-loss transition failed");
    check(backend.update(controller, false, false) && backend.grabs == 0,
          "Closing while unfocused stole capture from another application");
    check(controller.active(), "Deferred restoration was incorrectly reported idle");
    check(backend.update(controller, false, false) && backend.grabs == 0,
          "Unfocused update unexpectedly restored capture");
    check(backend.update(controller, true, false), "Reopening while a restore was deferred failed");
    check(backend.update(controller, true, true) && backend.grabs == 0,
          "Regaining focus while GUI open restored capture prematurely");
    check(backend.update(controller, false, true) && backend.grabs == 1,
          "Deferred original grab was lost across focus loss and reopening");

    check(backend.update(controller, true), "Second opening failed");
    backend.grabbed = true; // The host restores on its own as the GUI closes.
    check(backend.update(controller, false) && backend.grabs == 1,
          "Already-restored host state caused a duplicate native grab");
}

void retry_failed_operations() {
    MouseCaptureController controller;
    Backend backend;
    backend.release_succeeds = false;
    check(!backend.update(controller, true) && backend.releases == 1, "Failed release was reported successful");
    check(!backend.update(controller, true) && backend.releases == 2, "Failed native release was not retried");
    backend.release_succeeds = true;
    check(backend.update(controller, true) && backend.releases == 3, "Recovered native release was not retried");
    backend.grab_succeeds = false;
    check(!backend.update(controller, false) && backend.grabs == 1, "Failed restoration was reported successful");
    check(backend.update(controller, false, false) && backend.grabs == 1,
          "Failed restoration retried while unfocused");
    backend.grab_succeeds = true;
    check(backend.update(controller, false) && backend.grabs == 2, "Failed restoration was not retried after focus returned");

    MouseCaptureController unknown;
    unsigned attempts = 0;
    const auto release = [&] { return ++attempts == 2; };
    check(!unknown.update(true, true, std::nullopt, release, {}), "Unknown failed release was hidden");
    check(unknown.update(true, true, std::nullopt, release, {}) && attempts == 2,
          "Unknown-state release failure did not remain pending");
    check(unknown.update(false, true, std::nullopt, release, {}), "Unknown state incorrectly required a grab after release retry");
}

void reset_respects_focus_and_retries() {
    MouseCaptureController controller;
    Backend backend;
    check(backend.update(controller, true), "Opening before focused reset failed");
    backend.grab_succeeds = false;
    check(!backend.reset(controller, true) && backend.grabs == 1, "Failed focused reset was hidden");
    check(controller.active(), "Failed focused reset lost its pending restore obligation");
    backend.grab_succeeds = true;
    check(backend.reset(controller, true) && backend.grabs == 2, "Failed focused reset did not retain restoration for retry");
    check(!controller.active(), "Successful reset did not end capture management");
    check(backend.reset(controller, true) && backend.grabs == 2, "Reset restored capture more than once");

    check(backend.update(controller, true), "Opening before unfocused reset failed");
    check(backend.reset(controller, false) && backend.grabs == 2, "Unfocused reset stole native capture");
    check(!controller.active(), "Unfocused reset did not cancel capture management");
    check(backend.update(controller, false) && backend.grabs == 2,
          "Unfocused shutdown left an unexpected future restore obligation");
    check(backend.reset(controller, true) && backend.grabs == 2, "Reset without an original grab invoked native grab");
}

void unavailable_and_throwing_backends() {
    MouseCaptureController controller;
    const std::function<bool()> throws = []() -> bool { throw std::runtime_error("native operation failed"); };
    const std::function<bool()> succeeds = [] { return true; };
    check(!controller.update(true, true, true, {}, succeeds), "Missing required release callback was accepted");
    check(!controller.update(true, true, true, throws, succeeds), "Throwing release callback escaped or succeeded");
    check(controller.update(true, true, true, succeeds, succeeds), "Release did not recover after a callback exception");
    check(!controller.update(false, true, false, succeeds, {}), "Missing required grab callback was accepted");
    check(!controller.reset(true, throws), "Throwing reset callback escaped or succeeded");
    check(controller.reset(true, succeeds), "Reset did not recover after a callback exception");
}

void native_polling_is_independent_of_frame_rate() {
    for (const unsigned frame_ms : {1U, 4U, 16U}) {
        MouseSyncSchedule schedule;
        check(!schedule.update(false, false, 0), "Closed idle GUI polled native capture");
        check(schedule.update(true, false, 0), "Opening did not synchronize immediately");
        unsigned polls = 1;
        for (unsigned time = frame_ms; time < 1000; time += frame_ms)
            if (schedule.update(true, true, time)) ++polls;
        check(polls <= 10, "High frame rate caused more than ten native polls per second");
        check(schedule.update(false, true, 1000), "Closing waited for the polling interval");
        check(!schedule.update(false, false, 2000), "Restored closed GUI kept polling native capture");
    }
}

void raw_motion_is_not_clickgui_wheel() {
    using medicon::platform::genuine_legacy_wheel_delta;
    using medicon::platform::max_genuine_wheel_delta;
    using medicon::platform::raw_mouse_hwheel_flag;
    using medicon::platform::raw_mouse_wheel_flag;
    using medicon::platform::wheel_from_raw_mouse;

    const auto motion = wheel_from_raw_mouse(0, 8000);
    check(!motion.vertical && !motion.horizontal, "Relative raw motion became wheel input");
    const auto absolute = wheel_from_raw_mouse(0x0001, static_cast<unsigned short>(-4000));
    check(!absolute.vertical && !absolute.horizontal, "A button packet without a wheel flag became wheel input");

    const auto vertical = wheel_from_raw_mouse(raw_mouse_wheel_flag, 120);
    check(vertical.vertical && vertical.vertical_delta == 120 && !vertical.horizontal, "A real vertical wheel packet was dropped");
    const auto horizontal = wheel_from_raw_mouse(raw_mouse_hwheel_flag, static_cast<unsigned short>(-120));
    check(horizontal.horizontal && horizontal.horizontal_delta == -120 && !horizontal.vertical,
          "A real horizontal wheel packet was dropped");
    const auto zero = wheel_from_raw_mouse(raw_mouse_wheel_flag, 0);
    check(!zero.vertical, "A zero wheel delta was treated as scrolling");

    check(genuine_legacy_wheel_delta(120) && genuine_legacy_wheel_delta(-30) &&
              genuine_legacy_wheel_delta(static_cast<short>(max_genuine_wheel_delta)),
          "A genuine wheel delta was rejected");
    check(!genuine_legacy_wheel_delta(0) && !genuine_legacy_wheel_delta(static_cast<short>(max_genuine_wheel_delta + 1)) &&
              !genuine_legacy_wheel_delta(-2401),
          "A raw-motion-sized wheel delta was accepted");
    const auto huge = wheel_from_raw_mouse(raw_mouse_wheel_flag, 32767);
    check(huge.vertical && !genuine_legacy_wheel_delta(huge.vertical_delta),
          "An unbounded raw delta was eligible to reach ClickGUI");
}

void relative_pointer_owns_overlay_motion() {
    using medicon::platform::relative_pointer_owns_motion;
    check(relative_pointer_owns_motion(true, true, true, false),
          "An open focused no-legacy stream did not own relative motion");
    check(!relative_pointer_owns_motion(false, true, true, false),
          "A closed overlay still owned relative motion");
    check(!relative_pointer_owns_motion(true, false, true, false),
          "An unfocused overlay still owned relative motion");
    check(!relative_pointer_owns_motion(true, true, false, false),
          "A legacy mouse stream was forced onto the relative accumulator");
    check(!relative_pointer_owns_motion(true, true, true, true),
          "An absolute raw packet was treated as relative motion");
    check(relative_pointer_owns_motion(true, true, true, false),
          "ClickGUI and the title menu must share the same relative-motion rule");
}

void relative_mouse_pointer_tracks_no_legacy_motion() {
    medicon::platform::RelativeMousePointer pointer;
    check(!pointer.active(), "A fresh relative pointer was active");
    // The actual failure had the OS cursor below the 1920x1009 client area.
    check(pointer.move(10, -5, 1920, 1009, 1071, 1056) && pointer.active() &&
              pointer.x() == 970 && pointer.y() == 499,
          "An off-client OS cursor did not seed relative motion at the viewport center");
    check(pointer.move(-20, 10, 1920, 1009, 1919, 1008) && pointer.x() == 950 && pointer.y() == 509,
          "A stale OS cursor overwrote an active relative stream");
    check(pointer.move(0, 0, 1920, 1009, 0, 0) && pointer.x() == 950 && pointer.y() == 509,
          "A zero-delta raw button packet reset the relative cursor");

    pointer.reset();
    check(!pointer.active() && pointer.x() == 0 && pointer.y() == 0,
          "Focus/menu reset retained an active or stale relative pointer");
    check(pointer.move(2, -3, 100, 80, 10, 20) && pointer.x() == 12 && pointer.y() == 17,
          "A valid client seed was not used after reset");
    pointer.reset(); // Focus returns: the new OS position must replace the old stream.
    check(pointer.move(0, 0, 100, 80, 99, 79) && pointer.x() == 99 && pointer.y() == 79,
          "Focus regain did not reseed the relative cursor");
    check(pointer.move(-10, -10, 40, 30, 99, 79) && pointer.x() == 29 && pointer.y() == 19,
          "Resize retained overshoot instead of clamping before relative motion");
    check(pointer.move(1000, 1000, 40, 30, 0, 0) && pointer.x() == 39 && pointer.y() == 29,
          "Relative motion escaped the right/bottom viewport bounds");
    check(pointer.move(-1000, -1000, 40, 30, 0, 0) && pointer.x() == 0 && pointer.y() == 0,
          "Relative motion escaped the left/top viewport bounds");

    constexpr auto largest = std::numeric_limits<int>::max();
    constexpr auto smallest = std::numeric_limits<int>::min();
    pointer.reset();
    check(pointer.move(largest, largest, largest, largest, largest - 1, largest - 1) &&
              pointer.x() == largest - 1 && pointer.y() == largest - 1,
          "Relative addition overflowed signed coordinates");
    check(pointer.move(smallest, smallest, largest, largest, 0, 0) && pointer.x() == 0 && pointer.y() == 0,
          "Extreme negative relative motion overflowed or escaped the viewport");

    check(!pointer.move(1, 1, 0, 20, 0, 0) && !pointer.active() && pointer.x() == 0 && pointer.y() == 0,
          "A zero-width viewport did not reset relative cursor state");
    check(!pointer.move(1, 1, 20, -1, 0, 0) && !pointer.active(),
          "A negative-height viewport accepted relative motion");
    check(pointer.move(0, 0, 100, 80, 100, 0) && pointer.x() == 50 && pointer.y() == 40,
          "A seed on the exclusive viewport boundary was accepted");
    pointer.reset();
    check(pointer.move(0, 0, 100, 80, -1, 40) && pointer.x() == 50 && pointer.y() == 40,
          "A partially invalid seed was not replaced with the viewport center");
    check(pointer.move(0, 0, 1, 1, 99, 99) && pointer.x() == 0 && pointer.y() == 0,
          "A one-pixel viewport did not clamp relative coordinates");
}

#ifdef _WIN32
void direct_raw_mouse_reads_are_stationary_for_the_game() {
    RAWINPUT raw{};
    raw.header.dwType = RIM_TYPEMOUSE;
    raw.header.dwSize = sizeof(raw);
    raw.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE | MOUSE_VIRTUAL_DESKTOP;
    raw.data.mouse.lLastX = 24000;
    raw.data.mouse.lLastY = 12000;
    raw.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_WHEEL;
    raw.data.mouse.usButtonData = 120;
    medicon::platform::suppress_raw_mouse(raw);
    check(raw.header.dwType == RIM_TYPEMOUSE && raw.header.dwSize == sizeof(raw),
          "Mouse suppression invalidated the raw packet header");
    check(raw.data.mouse.usFlags == 0 && raw.data.mouse.lLastX == 0 && raw.data.mouse.lLastY == 0 &&
          raw.data.mouse.usButtonFlags == 0 && raw.data.mouse.usButtonData == 0,
          "A direct mouse read retained camera, attack or wheel input");
    raw.header.dwType = RIM_TYPEKEYBOARD;
    raw.data.keyboard.VKey = 'W';
    medicon::platform::suppress_raw_mouse(raw);
    check(raw.data.keyboard.VKey == 'W', "Mouse suppression modified a movement keyboard packet");
}

void only_pointer_motion_reuses_published_menu_state() {
    using medicon::platform::menu_input_requires_screen_validation;
    for (const UINT message : {WM_MOUSEMOVE, WM_NCMOUSEMOVE, WM_SETCURSOR})
        check(!menu_input_requires_screen_validation(message), "A pure pointer update required a full screen traversal");
    // Every interaction and lifecycle boundary retains a fresh screen check.
    for (const UINT message : {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK,
            WM_RBUTTONDOWN, WM_RBUTTONUP, WM_RBUTTONDBLCLK, WM_MBUTTONDOWN, WM_MBUTTONUP,
            WM_MBUTTONDBLCLK, WM_XBUTTONDOWN, WM_XBUTTONUP, WM_XBUTTONDBLCLK,
            WM_MOUSEWHEEL, WM_MOUSEHWHEEL, WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP,
            WM_CHAR, WM_SYSCHAR, WM_IME_COMPOSITION, WM_SETFOCUS, WM_KILLFOCUS, WM_ACTIVATE,
            WM_ACTIVATEAPP, WM_MOUSELEAVE, WM_CAPTURECHANGED, WM_INPUT_DEVICE_CHANGE,
            WM_SIZE, WM_CLOSE, WM_NULL, WM_USER + 17, WM_APP + 23})
        check(menu_input_requires_screen_validation(message), "An interaction or lifecycle message bypassed menu validation");
    check(menu_input_requires_screen_validation((std::numeric_limits<UINT>::max)()),
          "An unknown message bypassed menu validation");
    check(menu_input_requires_screen_validation(WM_INPUT), "Undecoded raw input bypassed menu validation");

    RAWINPUT raw{};
    raw.header.dwType = RIM_TYPEMOUSE;
    unsigned validations = 0;
    for (int event = 0; event != 1000; ++event) {
        raw.data.mouse.lLastX = event % 11 - 5;
        raw.data.mouse.lLastY = event % 7 - 3;
        validations += menu_input_requires_screen_validation(WM_INPUT, &raw) ? 1U : 0U;
    }
    check(validations == 0, "A 1000-event raw motion stream performed full screen validation");
    raw.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
    check(!menu_input_requires_screen_validation(WM_INPUT, &raw), "Button-free absolute raw motion required screen validation");
    for (const USHORT buttons : {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP,
            RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_DOWN,
            RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP,
            RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, RI_MOUSE_WHEEL, RI_MOUSE_HWHEEL,
            RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_WHEEL, 0x8000}) {
        raw.data.mouse.usButtonFlags = buttons;
        check(menu_input_requires_screen_validation(WM_INPUT, &raw), "A raw button/wheel packet bypassed menu validation");
    }
    raw.data.mouse.usButtonFlags = 0;
    raw.header.dwType = RIM_TYPEKEYBOARD;
    check(menu_input_requires_screen_validation(WM_INPUT, &raw), "Raw keyboard input bypassed menu validation");
    raw.header.dwType = RIM_TYPEHID;
    check(menu_input_requires_screen_validation(WM_INPUT, &raw), "Raw HID input bypassed menu validation");
    raw.header.dwType = RIM_TYPEMOUSE;
    check(menu_input_requires_screen_validation(WM_KEYDOWN, &raw) &&
              menu_input_requires_screen_validation(WM_LBUTTONDOWN, &raw),
          "An unrelated decoded motion packet suppressed a message's required validation");
}
#endif

void polling_keeps_transitions_and_retries_responsive() {
    MouseSyncSchedule schedule;
    MouseCaptureController controller;
    Backend backend;
    check(schedule.update(true, false, 10) && backend.update(controller, true), "Opening did not release capture");
    backend.grabbed = true; // A session/native transition changed capture between polls.
    check(!schedule.update(true, controller.active(), 109), "Native discovery ran before the retry interval");
    check(schedule.update(true, controller.active(), 110) && backend.update(controller, true) && backend.releases == 2,
          "Periodic synchronization did not recover a changed native capture state");
    backend.grab_succeeds = false;
    check(schedule.update(false, controller.active(), 111) && !backend.update(controller, false),
          "Closing did not immediately attempt restoration between polls");
    check(!schedule.update(false, controller.active(), 210), "Pending restore retried every frame");
    backend.grab_succeeds = true;
    check(schedule.update(false, controller.active(), 211) && backend.update(controller, false) && backend.grabs == 2,
          "Failed restoration did not receive a bounded retry");
    check(!schedule.update(false, controller.active(), 1000), "Successful restoration left native polling active");
    check(schedule.update(true, false, 1001) && schedule.update(false, true, 1002) && schedule.update(true, true, 1003),
          "Rapid GUI transitions were delayed by native polling");
}
void disconnect_account_picker_keeps_native_cursor() {
    MouseCaptureController controller;
    unsigned releases=0,grabs=0;
    const auto release=[&]{++releases;return true;};
    const auto grab=[&]{++grabs;return true;};
    check(controller.update(true,true,true,release,grab),"gameplay capture setup failed");
    check(releases==1 && controller.active(),"capture setup did not retain restoration");
    // The game has disconnected while an old gameplay restore is still owed.
    check(controller.update(true,true,true,release,grab,true),"disconnect picker did not relinquish native capture policy");
    check(releases==1 && grabs==0 && !controller.active(),"disconnect picker called native release/grab");
    check(controller.update(false,true,false,release,grab,true),"account completion could not close picker");
    check(grabs==0 && !controller.active(),"account completion restored a stale gameplay mouse");
    check(controller.update(false,true,false,release,grab),"leaving disconnect screen failed");
    check(grabs==0,"stale restore survived leaving disconnect screen");
}

} // namespace

int main() {
    try {
        opening_and_reassertion();
        overlay_press_keeps_its_owner_until_release();
        preserve_already_free_inventory();
        disconnect_account_picker_keeps_native_cursor();
        unknown_state_is_conservative();
        focus_defers_restoration();
        retry_failed_operations();
        reset_respects_focus_and_retries();
        unavailable_and_throwing_backends();
        native_polling_is_independent_of_frame_rate();
        polling_keeps_transitions_and_retries_responsive();
        raw_motion_is_not_clickgui_wheel();
        relative_pointer_owns_overlay_motion();
        relative_mouse_pointer_tracks_no_legacy_motion();
#ifdef _WIN32
        direct_raw_mouse_reads_are_stationary_for_the_game();
        only_pointer_motion_reuses_published_menu_state();
#endif
        std::cout << "Mouse capture transition tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mouse capture: " << error.what() << '\n';
        return 1;
    }
}
