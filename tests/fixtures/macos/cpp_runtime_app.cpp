#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "cpp_runtime_abi.hpp"
extern "C" int puts(const char*);
extern "C" void alchemy_cpp_throw(int*);
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    std::vector<int> values{27, 42};
    std::map<std::string, int> entries;
    std::mutex mutex;
    std::atomic<int> completed{0};
    const auto work = [&](const char* name, int value) {
        std::lock_guard<std::mutex> lock(mutex);
        entries[std::string(name)] = value;
        ++completed;
    };
    std::thread first(work, "first", values[0]);
    std::thread second(work, "second", values[1]);
    first.join(); second.join();
    if (completed != 2 || entries["first"] != 27 || entries["second"] != 42) return 2;
    int cleaned = 0;
    try { alchemy_cpp_throw(&cleaned); return 3; }
    catch (const std::runtime_error& error) {
        if (std::string(error.what()) != "cross-dylib C++ exception: 27" || cleaned != 1) return 4;
    }
    std::ostringstream formatted;
    formatted << "Guest C++ runtime: " << std::accumulate(values.begin(), values.end(), 0) << '\n';
    {
        std::ofstream output(argv[1], std::ios::binary);
        output << formatted.str();
        if (!output) return 5;
    }
    const auto size = std::filesystem::file_size(argv[1]);
    if (size != formatted.str().size()) return 6;
    std::ifstream input(argv[1], std::ios::binary);
    std::string line;
    std::getline(input, line);
    if (line != "Guest C++ runtime: 69") return 7;
    puts(line.c_str());
    return 0;
}
