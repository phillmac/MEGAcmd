#!/usr/bin/env python3
"""Compile and exercise the actual checkAndInformPSA function with fake SDK callbacks.

Set CXX and CXXFLAGS for a non-system compiler, otherwise use g++.
No MEGA credentials, servers, network, or filesystem state are touched.
"""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
executer = (ROOT / "src/megacmdexecuter.cpp").read_text()
start = executer.index("bool MegaCmdExecuter::checkAndInformPSA(")
end = executer.index("\nstd::string MegaCmdExecuter::formatErrorAndMaySetErrorCode", start)
function = executer[start:end]

stub = r"""
#include "psa_async_gate.h"
#include <cassert>
#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
using namespace std;
using namespace std::chrono_literals;
using mega_time = time_t;
using m_time_t = time_t;
m_time_t m_time() { return std::time(nullptr); }
static std::ostringstream sink;
#define LOG_debug sink
#define LOG_verbose sink
#define LOG_warn sink
namespace megacmd {
using ::m_time; using ::m_time_t;
struct ConfigurationManager {
    static bool autoPsaFetch;
    template<typename T>
    static T getConfigurationValue(const char*, T) { return static_cast<T>(autoPsaFetch); }
};
bool ConfigurationManager::autoPsaFetch = false;
struct MegaUser {};
struct MegaError {
    static constexpr int API_OK = 0;
    static constexpr int API_ENOENT = -9;
    int code = API_OK;
    int getErrorCode() const { return code; }
    const char* getErrorString() const { return "error"; }
};
struct MegaRequest {
    long getNumber() const { return 77; }
    const char* getName() const { return "Example"; }
    const char* getText() const { return "announcement"; }
    const char* getPassword() const { return ""; }
    const char* getLink() const { return ""; }
};
struct MegaApi;
struct MegaCmdListenerFuncExecuter {
    using Callback = std::function<void(MegaApi*, MegaRequest*, MegaError*)>;
    Callback callback;
    bool autoremove;
    MegaCmdListenerFuncExecuter(Callback c, bool a) : callback(std::move(c)), autoremove(a) {}
    void finish(MegaApi* api, MegaRequest* req, MegaError* e) {
        callback(api, req, e);
        if (autoremove) delete this;
    }
};
struct MegaApi {
    MegaCmdListenerFuncExecuter *listener = nullptr;
    int submitted = 0;
    bool loggedIn = true;
    MegaUser* getMyUser() { return loggedIn ? new MegaUser : nullptr; }
    void getPSA(MegaCmdListenerFuncExecuter *l) {
        assert(!listener);
        listener=l;
        ++submitted;
    }
    void completePSA(int code = MegaError::API_OK) {
        assert(listener);
        auto *l=listener;
        listener=nullptr;
        MegaRequest request;
        MegaError error{code};
        l->finish(this,&request,&error);
    }
    ~MegaApi() { delete listener; }
};
struct MegaCmdSandbox {
    m_time_t timeOfPSACheck=0;
    int lastPSAnumreceived=0;
};
std::string notification;
void informStateListener(const string& s, int) { notification=s; }
void broadcastMessage(const string& s) { notification=s; }
class MegaCmdExecuter {
public:
    MegaApi *api;
    MegaCmdSandbox *sandboxCMD;
    std::shared_ptr<PsaAsyncGate> mPsaGate=std::make_shared<PsaAsyncGate>();
    MegaCmdExecuter(MegaApi &apiArg, MegaCmdSandbox &s) : api(&apiArg), sandboxCMD(&s) {}
    bool checkAndInformPSA(std::optional<int> clientID, bool enforce=false);
};
"""
main = r"""
int main()
{
    MegaApi api;
    MegaCmdSandbox sandbox;
    MegaCmdExecuter exec(api,sandbox);
    // Default OFF: no automatic SDK request, but explicit fetch still works.
    assert(!exec.checkAndInformPSA(std::nullopt));
    assert(api.submitted==0);
    assert(exec.checkAndInformPSA(std::nullopt, true));
    assert(api.submitted==1);
    assert(notification.empty()); // Does not synchronously wait for callback.
    assert(!exec.checkAndInformPSA(3, true)); // No unbounded listener growth.
    assert(api.submitted==1);
    api.completePSA();
    assert(sandbox.lastPSAnumreceived==77);
    assert(notification.find("Example")!=string::npos);
    notification.clear();

    // Enabling immediately admits the normal, non-forced startup path.
    ConfigurationManager::autoPsaFetch = true;
    sandbox.timeOfPSACheck = 0;
    assert(exec.checkAndInformPSA(std::nullopt));
    assert(api.submitted==2);
    api.completePSA(MegaError::API_ENOENT);
    assert(notification.empty());

    // Disabling it again does not suppress explicit manual requests.
    ConfigurationManager::autoPsaFetch = false;
    sandbox.timeOfPSACheck = 0;
    assert(!exec.checkAndInformPSA(std::nullopt));
    assert(api.submitted==2);
    assert(exec.checkAndInformPSA(std::nullopt,true));
    api.completePSA(MegaError::API_ENOENT);
    assert(notification.empty());
    assert(exec.checkAndInformPSA(std::nullopt,true));
    exec.mPsaGate->stop();
    api.completePSA(); // callback cannot access sandbox after server shutdown.
    assert(notification.empty());
    assert(!exec.checkAndInformPSA(std::nullopt,true));
    MegaApi neverCompletes;
    MegaCmdSandbox anotherSandbox;
    MegaCmdExecuter another(neverCompletes, anotherSandbox);
    assert(another.checkAndInformPSA(std::nullopt,true));
    auto before=chrono::steady_clock::now();
    another.mPsaGate->stop();
    assert(chrono::steady_clock::now()-before < 1s);
    return 0;
}
} // namespace megacmd
int main() { return megacmd::main(); }
"""
assert "megaCmdListener->wait()" not in function, "PSA still synchronously waits"
with tempfile.TemporaryDirectory(prefix="megacmd-psa-fake-sdk-") as tmp:
    source = Path(tmp) / "check.cpp"
    executable = Path(tmp) / "check"
    source.write_text(stub + function + main)
    compiler = shlex.split(os.environ.get("CXX", "g++"))
    flags = shlex.split(os.environ.get("CXXFLAGS", "-std=c++17 -pthread"))
    subprocess.run(
        compiler + flags + ["-I", str(ROOT / "src"), str(source), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True, timeout=10)
print("PASS fake-SDK integration: actual PSA function, callbacks, duplicate guard, teardown")
