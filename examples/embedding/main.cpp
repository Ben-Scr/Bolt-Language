#include <bolt/Bolt.hpp>
#include <iostream>
int main() {
    try {
        auto program = bolt::compile_source(R"(
            public class Counter {
            public:
                int Value;
                void Add(int amount) { Value += amount; }
            }
            static class Calculator {
            public:
                int Twice(int value) { return value * 2; }
            }
        )", "embedded.bolt", false);
        bolt::Runtime runtime(std::move(program));
        const auto result = runtime.call("Calculator", "Twice", {bolt::Value(std::int32_t{21})});
        std::cout << "Result from Bolt: " << result.as_int() << '\n';
        auto counter = runtime.create("Counter");
        runtime.call(counter, "Add", {bolt::Value(std::int32_t{5})});
        std::cout << "Counter value: " << counter.object().fields.at("Value").as_int() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
