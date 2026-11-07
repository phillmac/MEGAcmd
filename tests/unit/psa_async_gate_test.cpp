#include "../../src/psa_async_gate.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using megacmd::PsaAsyncGate;
using namespace std::chrono_literals;

void testRequestNeverCompletes()
{
    auto gate = std::make_shared<PsaAsyncGate>();
    std::atomic<int> normalCommands{0};
    assert(gate->tryStart());
    assert(!gate->tryStart()); // Do not allocate more SDK listeners.
    for (int i = 0; i < 100000; ++i)
    {
        ++normalCommands; // Dispatcher makes progress with an absent callback.
    }
    assert(normalCommands == 100000);
    gate->stop(); // Must not wait for an SDK request that never finishes.
    assert(!gate->tryStart());
}

void testCompletionAndRetry()
{
    auto gate = std::make_shared<PsaAsyncGate>(2s);
    int delivered = 0;
    assert(gate->tryStart());
    assert(gate->finish([&] { ++delivered; }));
    assert(delivered == 1);
    assert(gate->tryStart()); // Completed request releases the slot.
    assert(gate->finish([&] { ++delivered; }));
    assert(delivered == 2);
    assert(!gate->finish([&] { ++delivered; }));
    assert(delivered == 2);
}

void testLateCompletionIgnored()
{
    auto gate = std::make_shared<PsaAsyncGate>(1ms);
    assert(gate->tryStart());
    std::this_thread::sleep_for(4ms);
    int delivered = 0;
    assert(!gate->finish([&] { ++delivered; }));
    assert(delivered == 0);
    assert(gate->tryStart());
}

void testStopRejectsLateCallbacks()
{
    auto gate = std::make_shared<PsaAsyncGate>();
    std::weak_ptr<PsaAsyncGate> weak = gate;
    assert(gate->tryStart());
    gate->stop();
    int delivered = 0;
    if (auto live = weak.lock())
    {
        assert(!live->finish([&] { ++delivered; }));
    }
    gate.reset();
    assert(!weak.lock()); // Late SDK callback cannot access server lifetime.
    assert(delivered == 0);
}

void testTeardownWaitsForInProgressDelivery()
{
    auto gate = std::make_shared<PsaAsyncGate>(2s);
    assert(gate->tryStart());
    std::promise<void> entered, release;
    auto ready = entered.get_future();
    auto releaseFuture = release.get_future();
    std::thread callback([&]
    {
        assert(gate->finish([&]
        {
            entered.set_value();
            releaseFuture.wait();
        }));
    });
    ready.wait();
    std::atomic<bool> stopped{false};
    std::thread teardown([&] { gate->stop(); stopped = true; });
    std::this_thread::sleep_for(10ms);
    assert(!stopped); // stop cannot destroy state during live callback.
    release.set_value();
    callback.join();
    teardown.join();
    assert(stopped);
}

int main()
{
    testRequestNeverCompletes();
    testCompletionAndRetry();
    testLateCompletionIgnored();
    testStopRejectsLateCallbacks();
    testTeardownWaitsForInProgressDelivery();
    std::cout << "PASS 5 PSA asynchronous lifecycle regression cases\n";
}
