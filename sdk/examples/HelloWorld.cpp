#include <medicon.hpp>
using namespace medicon;

class HelloWorld : public Module {
    void on_enable() override { chat::print("Hello, Medicon!"); }
};
MEDICON_MODULE(HelloWorld, "hello", "Hello World", Category::Utility);
