#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned startup_order;
static unsigned shutdown_order;
alignas(32) static thread_local unsigned initialized = 17;
alignas(64) static thread_local unsigned zero_initialized;

static void early_setup() __attribute__((constructor(200)));
static void late_cleanup() __attribute__((destructor(200)));

static void early_setup() { startup_order = 1; }

static void late_cleanup()
{
    if (shutdown_order != 3)
        _Exit(3);
    puts("cxx finalization ok");
}

struct Lifecycle {
    Lifecycle() { startup_order = startup_order * 10 + 2; }
    ~Lifecycle() { shutdown_order = shutdown_order * 10 + 3; }
};

static Lifecycle lifecycle;

static bool tls_is_fresh()
{
    return initialized == 17 && zero_initialized == 0 &&
        reinterpret_cast<uintptr_t>(&initialized) % 32 == 0 &&
        reinterpret_cast<uintptr_t>(&zero_initialized) % 64 == 0;
}

static void *worker(void *result)
{
    *static_cast<bool *>(result) = tls_is_fresh();
    initialized = 90;
    zero_initialized = 91;
    return nullptr;
}

int main()
{
    if (startup_order != 12 || !tls_is_fresh())
        return 1;

    initialized = 20;
    zero_initialized = 21;
    pthread_t thread;
    bool fresh = false;
    if (pthread_create(&thread, nullptr, worker, &fresh) || pthread_join(thread, nullptr) ||
        !fresh || initialized != 20 || zero_initialized != 21)
        return 2;

    puts("cxx startup and TLS ok");
    return 0;
}
