#!/usr/bin/env python3
"""Compile and exercise the REAL MegaCmdExecuter 'psa' command branch with a fake SDK.

No MEGA network, credentials, live daemon, or state files are involved.
CXX and CXXFLAGS allow using an isolated C++ toolchain.
"""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
src = (root / "src/megacmdexecuter.cpp").read_text()
start = src.index('    else if (words[0] == "psa")')
end = src.index('    else if (words[0] == "mount")', start)
psa_branch = src[start:end]
assert 'savePropertyValue("autoPsaFetch", enabled)' in psa_branch
assert 'checkAndInformPSA(std::nullopt, true)' in psa_branch
parser = (root / "src/megacmd.cpp").read_text()
psa_options = parser.split('else if ("psa" == thecommand)', 1)[1].split('else if', 1)[0]
assert 'validOptValues->insert("auto");' in psa_options
assert 'validParams->insert("discard");' in psa_options

harness = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
using namespace std;
constexpr int MCMD_EARGS=2;
#define SSTR(x) std::to_string(x)
namespace megacmd {
struct MegaError {};
struct MegaCmdListener {
    MegaCmdListener(void*) {}
    void wait() {}
    MegaError* getError() { return nullptr; }
};
struct MegaApi {
    int discarded=0;
    void setPSA(int, MegaCmdListener*) { ++discarded; }
};
struct MegaCmdSandbox { int lastPSAnumreceived=1; };
struct ConfigurationManager {
    static bool autoPsaFetch;
    static void savePropertyValue(const char* name, bool enabled) {
        assert(string(name)=="autoPsaFetch");
        autoPsaFetch=enabled;
    }
    template<class T> static T getConfigurationValue(const char* name, T fallback) {
        assert(string(name)=="autoPsaFetch");
        return static_cast<T>(autoPsaFetch);
    }
};
bool ConfigurationManager::autoPsaFetch=false;
string getOption(map<string,string>* opts, const string& name, const string& fallback) {
    auto it=opts->find(name);
    return it==opts->end()?fallback:it->second;
}
bool getFlag(map<string,int>* flags,const string& name) {
    return flags->count(name) && flags->at(name)!=0;
}
int toInteger(const string& s, int fallback) {
    try { return stoi(s); } catch(...) { return fallback; }
}
struct MegaCmdExecuter {
    MegaApi *api;
    MegaCmdSandbox *sandboxCMD;
    ostringstream output;
    ostringstream errors;
    int errorCode=0;
    int manualChecks=0;
    explicit MegaCmdExecuter(MegaApi& a,MegaCmdSandbox& s):api(&a),sandboxCMD(&s) {}
    void setCurrentThreadOutCode(int e) { errorCode=e; }
    bool checkNoErrors(MegaError*, const string&) { return true; }
    bool checkAndInformPSA(optional<int>, bool enforce) {
        assert(enforce);
        ++manualChecks;
        return true;
    }
    void run(map<string,string> opts={},map<string,int> flags={}) {
        output.str(""); output.clear();
        errors.str("");errors.clear();
        errorCode=0;
        vector<string> words{"psa"};
        auto *cloptions=&opts;
        auto *clflags=&flags;
#define OUTSTREAM output
#define LOG_err errors
        if (false) {}
'''
footer = r'''
#undef OUTSTREAM
#undef LOG_err
    }
};
int main() {
    MegaApi api;
    MegaCmdSandbox sandbox;
    MegaCmdExecuter e(api,sandbox);
    assert(!ConfigurationManager::autoPsaFetch);

    e.run({{"auto","query"}});
    assert(e.output.str().find("disabled")!=string::npos);
    assert(e.manualChecks==0 && api.discarded==0);

    e.run();
    assert(e.manualChecks==1 && e.output.str().find("queued")!=string::npos);

    e.run({{"auto","ON"}});
    assert(ConfigurationManager::autoPsaFetch && e.manualChecks==1);
    e.run({{"auto","query"}});
    assert(e.output.str().find("enabled")!=string::npos);

    e.run({{"auto","off"}});
    assert(!ConfigurationManager::autoPsaFetch && e.manualChecks==1);
    e.run();
    assert(e.manualChecks==2); // Manual fetching still allowed with auto OFF.

    e.run({{"auto","broken"}});
    assert(e.errorCode==MCMD_EARGS && e.manualChecks==2);
    e.run({{"auto",""}});
    assert(e.errorCode==MCMD_EARGS && e.manualChecks==2);

    e.run({{"auto","off"}}, {{"discard",1}});
    assert(e.errorCode==MCMD_EARGS && api.discarded==0);
    e.run({}, {{"discard",1}});
    assert(e.errorCode==0 && api.discarded==1 && e.manualChecks==3);
    e.run({{"auto","QUERY"}});
    assert(e.output.str().find("disabled")!=string::npos);
    return 0;
}
} // namespace megacmd
int main() { return megacmd::main(); }
'''
with tempfile.TemporaryDirectory(prefix="megacmd-psa-cli-") as tmp:
    f = Path(tmp) / "cli.cpp"
    exe = Path(tmp) / "cli"
    f.write_text(harness + psa_branch + footer)
    subprocess.run(
        shlex.split(os.getenv("CXX", "g++"))
        + shlex.split(os.getenv("CXXFLAGS", "-std=c++17 -pthread"))
        + [str(f), "-o", str(exe)],
        check=True,
    )
    subprocess.run([str(exe)], timeout=10, check=True)
print("PASS compiled real PSA command: auto ON/OFF/query, defaults, manual fetch, discard, errors")
