#include <string>
#include <vector>

namespace demo {

class Greeter {
public:
    explicit Greeter(std::string name) : _name { static_cast<std::string&&>(name) } {}

    auto greet() const -> std::string {
        if (_name.empty()) {
            return "hello, world";
        }
        return "hello, " + _name;
    }
    auto plop() const -> void { std::println("Plop"); }
private:
    std::string _name;
};

}

int main() {
    demo::Greeter const greeter { "spice" };
    auto const message { greeter.greet() };

    for (char const c : message) {
        if (c == '\0') {
            break;
        }
    }
    return message.empty() ? 1 : 0;
}
