#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>

void throw_from_other_file(unsigned &);

static bool exceptions_work()
{
    unsigned order = 0;
    struct Cleanup {
        unsigned &order;
        unsigned step;
        ~Cleanup() { order = order * 10 + step; }
    };

    try {
        Cleanup outer{order, 3};
        try {
            Cleanup inner{order, 2};
            throw_from_other_file(order);
        } catch (const std::runtime_error &error) {
            if (order != 12 || std::string(error.what()) != "mario exception")
                return false;
            throw;
        }
    } catch (const std::runtime_error &error) {
        return order == 123 && std::string(error.what()) == "mario exception";
    }
    return false;
}

struct Base {
    virtual ~Base() = default;
};
struct Derived : Base {};
struct Unrelated : Base {};

static bool __attribute__((noinline, noipa)) rtti_works(Base &value)
{
    return dynamic_cast<Derived *>(&value) && !dynamic_cast<Unrelated *>(&value) &&
        typeid(value) == typeid(Derived);
}

static std::atomic<unsigned> ready{0};
static std::atomic<unsigned> initializations{0};
static std::atomic<unsigned> destructors{0};
static std::atomic<unsigned> destructor_ids{0};

static const std::string &guarded()
{
    static const std::string value = [] {
        ++initializations;
        std::this_thread::yield();
        return std::string("initialized once");
    }();
    return value;
}

static bool thread_local_works(unsigned id)
{
    struct ThreadState {
        unsigned id = 0;
        ~ThreadState()
        {
            ++destructors;
            destructor_ids.fetch_or(id);
        }
    };
    static thread_local ThreadState state;
    const bool fresh = state.id == 0;
    state.id = id;

    ++ready;
    while (ready.load() < 2)
        std::this_thread::yield();

    return fresh && guarded() == "initialized once" && state.id == id && exceptions_work();
}

static bool check_finalization;
struct GlobalState {
    ~GlobalState()
    {
        if (check_finalization)
            std::puts("cxx global destructor ok");
    }
};
static GlobalState global_state;

static void leave_noexcept() noexcept
{
    std::set_terminate([] {
        if (!std::current_exception() || std::uncaught_exceptions() != 0)
            std::exit(3);
        std::puts("cxx noexcept handler ok");
        std::exit(42);
    });
    unsigned order = 0;
    throw_from_other_file(order);
}

int main(int argc, char **)
{
    if (argc > 1) {
        leave_noexcept();
        return 4;
    }

    Derived value;
    if (!rtti_works(value) || !exceptions_work())
        return 1;

    bool first = false;
    bool second = false;
    std::thread first_thread([&] { first = thread_local_works(1); });
    std::thread second_thread([&] { second = thread_local_works(2); });
    first_thread.join();
    second_thread.join();
    if (!first || !second || initializations != 1 || destructors != 2 || destructor_ids != 3)
        return 2;

    std::puts("cxx exceptions, RTTI, and thread-local destructors ok");
    check_finalization = true;
    return 0;
}
