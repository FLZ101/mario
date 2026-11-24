#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static const std::string &message()
{
    static const std::string text = "mario stdlib";
    return text;
}

int main()
{
    auto values = std::make_unique<std::vector<std::string>>();
    values->push_back(message());
    values->emplace_back("works");
    std::ostringstream stream;
    stream << values->at(0) << ' ' << values->at(1);
    if (stream.str() != "mario stdlib works")
        return 1;

    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    std::thread worker([&] {
        std::lock_guard<std::mutex> guard(mutex);
        ready = true;
        changed.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return ready; });
    }
    worker.join();

    std::unique_lock<std::mutex> lock(mutex);
    const auto start = std::chrono::steady_clock::now();
    if (changed.wait_for(lock, std::chrono::milliseconds(30), [] { return false; }) ||
        std::chrono::steady_clock::now() - start < std::chrono::milliseconds(30))
        return 2;

    std::cout << "cxx stdlib and threads ok\n";
    return 0;
}
