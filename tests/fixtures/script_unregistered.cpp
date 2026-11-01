#include <medicon.hpp>

// A valid C++ translation unit/DLL, but not a registered Medicon script.
// Keep this export-free on both MSVC and LLVM-MinGW.
class Hello : public medicon::Module {
    void on_enable() override { medicon::chat::print("Hello"); }
};
