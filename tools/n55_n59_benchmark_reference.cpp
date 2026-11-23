#include <cstdint>

int main() {
    volatile std::int64_t total = 0;
    for (std::int64_t index = 0; index < 500000; ++index) {
        total = total + index;
    }
    return total == 124999750000LL ? 42 : 1;
}
