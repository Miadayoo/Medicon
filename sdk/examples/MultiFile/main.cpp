#include <medicon.hpp>
#include "helpers.hpp"
using namespace medicon;
class MultiFile : public Module {
    void on_enable() override { chat::print(greeting()); }
};
MEDICON_MODULE(MultiFile, "multi_file", "Multi File", Category::Utility);
