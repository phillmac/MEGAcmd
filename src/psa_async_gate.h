/**
 * @file psa_async_gate.h
 * @brief Lifetime and bounded-in-flight guard for asynchronous PSA notifications.
 *
 * PSA requests are advisory. The SDK may never complete them. Never block the
 * petition dispatcher (or teardown) waiting for an SDK PSA completion.
 */
#pragma once

#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>

namespace megacmd {

class PsaAsyncGate
{
public:
    // The real server always uses 30s. Debug/test builds may shorten the
    // delivery window to exercise a real SDK response arriving after expiry
    // but *before* the SDK HTTP layer's own shorter request timeout.
    static std::chrono::steady_clock::duration defaultDeadline()
    {
#ifdef MEGACMD_TESTING_CODE
        if (const char* value = std::getenv("MEGACMD_TEST_PSA_GATE_DEADLINE_MS"))
        {
            char* end = nullptr;
            const long ms = std::strtol(value, &end, 10);
            if (end != value && end && *end == '\0' && ms >= 100 && ms <= 30000)
            {
                return std::chrono::milliseconds(ms);
            }
        }
#endif
        return std::chrono::seconds(30);
    }

    explicit PsaAsyncGate(
        std::chrono::steady_clock::duration deadline = defaultDeadline())
      : mDeadline(deadline)
    {
    }

    // Only one outstanding listener may exist, even if its SDK request stalls.
    // A request with no callback is deliberately NOT retried: allowing another
    // attempt every timeout would leak one listener per attempt indefinitely.
    bool tryStart()
    {
        std::lock_guard<std::mutex> guard(mMutex);
        if (mStopped || mPending)
        {
            return false;
        }
        mPending = true;
        mStarted = std::chrono::steady_clock::now();
        return true;
    }

    // The callback must not capture any server-owned object directly; it
    // acquires this gate via weak_ptr, then executes while holding the lock.
    // stop() waits for a running callback to finish before server teardown.
    bool finish(const std::function<void()>& deliver)
    {
        std::lock_guard<std::mutex> guard(mMutex);
        if (mStopped || !mPending)
        {
            return false;
        }
        mPending = false;
        if (std::chrono::steady_clock::now() - mStarted >= mDeadline)
        {
            return false; // Expired callback: do not notify clients.
        }
        deliver();
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> guard(mMutex);
        mStopped = true;
        // Do not delete a listener still owned by an outstanding SDK request.
        // Its callback retains only a weak_ptr to this gate and self-deletes
        // on SDK completion, even after the executor has been destroyed.
    }

private:
    std::mutex mMutex;
    std::chrono::steady_clock::time_point mStarted{};
    std::chrono::steady_clock::duration mDeadline;
    bool mPending = false;
    bool mStopped = false;
};

} // namespace megacmd
