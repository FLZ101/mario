#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

#define FAIL(what) do { std::fprintf(stderr, "cxx-concurrency: %s failed\n", what); return 1; } while (0)

int main()
{
    std::atomic<int> counter{0};
    const auto increment = [&] {
        for (int index = 0; index < 1000; ++index)
            counter.fetch_add(1, std::memory_order_relaxed);
    };
    std::thread first(increment);
    std::thread second(increment);
    first.join();
    second.join();
    if (counter.load() != 2000)
        FAIL("atomic fetch_add");
    int expected = 2000;
    if (!counter.compare_exchange_strong(expected, 0) || expected != 2000 || counter.load() != 0)
        FAIL("atomic compare_exchange");

    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    std::thread worker([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
        changed.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return ready; });
    }
    worker.join();
    if (!ready)
        FAIL("condition_variable");

    std::promise<int> promise;
    std::future<int> future = promise.get_future();
    std::thread setter([&] { promise.set_value(42); });
    if (future.get() != 42)
        FAIL("promise/future");
    setter.join();

    auto async_result = std::async(std::launch::async, [] { return 7; });
    if (async_result.get() != 7)
        FAIL("async");

    std::packaged_task<int(int)> task([](int value) { return value + 1; });
    std::future<int> packaged_future = task.get_future();
    std::thread task_thread(std::move(task), 5);
    if (packaged_future.get() != 6)
        FAIL("packaged_task");
    task_thread.join();

    std::once_flag once;
    int once_calls = 0;
    const auto call_once_worker = [&] {
        std::call_once(once, [&] { ++once_calls; });
    };
    call_once_worker();
    call_once_worker();
    call_once_worker();
    if (once_calls != 1)
        FAIL("call_once");

    int shared = 0;
    std::mutex first_mutex;
    std::mutex second_mutex;
    const auto scoped_worker = [&] {
        std::scoped_lock lock(first_mutex, second_mutex);
        ++shared;
    };
    std::thread scoped_first(scoped_worker);
    std::thread scoped_second(scoped_worker);
    scoped_first.join();
    scoped_second.join();
    if (shared != 2)
        FAIL("scoped_lock");

    std::puts("cxx-concurrency: atomics, futures, and synchronization ok");
    return 0;
}
