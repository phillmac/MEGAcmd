/**
 * (c) 2013 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of MEGAcmd.
 *
 * MEGAcmd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "MegaCmdTestingTools.h"
#include "Instruments.h"
#include "TestUtils.h"

using TI = TestInstruments;

TEST_F(NOINTERACTIVEBasicTest, Version)
{
    executeInClient({"version"});
}

TEST_F(NOINTERACTIVEBasicTest, Help)
{
    executeInClient({"help"});
}

TEST_F(NOINTERACTIVEBasicTest, PsaNeverCompletesDoesNotBlockPetitions)
{
    // Exercise the real in-process server and client petition dispatch,
    // without login or network requests. The fake PSA request never calls
    // back; ordinary commands must still complete, and the server shutdown
    // in integration main() must not wait for an absent SDK completion.
    TI::Instance().setTestValue(TI::TestValue::PSA_STALLED_REQUESTS_STARTED, uint64_t(0));
    TestInstrumentsFlagGuard sdkNeverCompletes(TI::Flag::PSA_SDK_NEVER_COMPLETES);

    auto first = executeInClient({"psa"});
    EXPECT_TRUE(first.ok()) << first.err();

    auto started = TI::Instance().testValue(TI::TestValue::PSA_STALLED_REQUESTS_STARTED);
    ASSERT_TRUE(started.has_value());
    EXPECT_EQ(std::get<uint64_t>(*started), uint64_t(1))
        << "PSA fault injection did not exercise the actual executor";

    // A second manual PSA command must not generate more outstanding listeners.
    auto second = executeInClient({"psa"});
    EXPECT_TRUE(second.ok()) << second.err();
    started = TI::Instance().testValue(TI::TestValue::PSA_STALLED_REQUESTS_STARTED);
    ASSERT_TRUE(started.has_value());
    EXPECT_EQ(std::get<uint64_t>(*started), uint64_t(1));

    for (int i = 0; i < 6; ++i)
    {
        const auto begin = std::chrono::steady_clock::now();
        auto response = executeInClient({"echo", "psa-dispatch-still-responsive"});
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        EXPECT_TRUE(response.ok()) << response.err();
        EXPECT_THAT(response.out(), testing::HasSubstr("psa-dispatch-still-responsive"));
        EXPECT_LT(elapsed, std::chrono::seconds(3))
            << "command processing waited behind a missing PSA completion";
    }
    auto version = executeInClient({"version"});
    EXPECT_TRUE(version.ok()) << version.err();

    TI::Instance().resetTestValue(TI::TestValue::PSA_STALLED_REQUESTS_STARTED);
}

TEST_F(NOINTERACTIVEBasicTest, PsaActualSdkTransportCanStallWithoutBlockingPetitions)
{
    // This is a real MegaApi -> SDK HTTP command test, not a synthetic
    // PsaAsyncGate fixture. The URL comes from a loopback-only test VM.
    if (!getenv("MEGACMD_TEST_GPSA_API_URL"))
    {
        GTEST_SKIP() << "loopback SDK API endpoint not configured";
    }

    TI::Instance().setTestValue(TI::TestValue::PSA_REAL_SDK_REQUESTS_STARTED, uint64_t(0));
    TI::Instance().setTestValue(TI::TestValue::PSA_REAL_SDK_LATE_COMPLETIONS_REJECTED, uint64_t(0));
    TestInstrumentsFlagGuard allowAnonymousSdkRequest(
        TI::Flag::PSA_ALLOW_ANONYMOUS_SDK_TRANSPORT);

    // A greeting may schedule the first PSA check before the manual petition
    // is parsed. Both scheduling orders are valid; at most one may be in flight.
    auto first = executeInClient({"psa"});
    ASSERT_TRUE(first.ok()) << first.err();
    EXPECT_THAT(first.out(), testing::AnyOf(
        testing::HasSubstr("PSA check queued"),
        testing::HasSubstr("already pending")));

    auto second = executeInClient({"psa"});
    ASSERT_TRUE(second.ok()) << second.err();
    EXPECT_THAT(second.out(), testing::HasSubstr("already pending"));

    auto count = TI::Instance().testValue(TI::TestValue::PSA_REAL_SDK_REQUESTS_STARTED);
    ASSERT_TRUE(count.has_value());
    ASSERT_TRUE(std::holds_alternative<uint64_t>(*count));
    EXPECT_EQ(std::get<uint64_t>(*count), uint64_t(1))
        << "the actual MegaApi::getPSA call must be submitted only once";

    for (int i = 0; i < 12; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        auto response = executeInClient({"echo", "sdk-http-pending-dispatch"});
        ASSERT_TRUE(response.ok()) << response.err();
        EXPECT_THAT(response.out(), testing::HasSubstr("sdk-http-pending-dispatch"));
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
    }
    auto version = executeInClient({"version"});
    EXPECT_TRUE(version.ok()) << version.err();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    count = TI::Instance().testValue(TI::TestValue::PSA_REAL_SDK_REQUESTS_STARTED);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(std::get<uint64_t>(*count), uint64_t(1));
    if (getenv("MEGACMD_TEST_GPSA_EXPECT_LATE_CALLBACK"))
    {
        // The mock returns API_ENOENT after 8 seconds; the test-only gate
        // deadline is 3 seconds. Production remains a 30-second window.
        // Avoid the SDK's ~20-second per-attempt HTTP timeout and retries.
        bool rejected = false;
        for (int i = 0; i < 80; ++i)
        {
            auto value = TI::Instance().testValue(
                TI::TestValue::PSA_REAL_SDK_LATE_COMPLETIONS_REJECTED);
            if (value && std::get<uint64_t>(*value) > 0)
            {
                rejected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        EXPECT_TRUE(rejected) << "late real SDK callback was never rejected";
    }
    TI::Instance().resetTestValue(TI::TestValue::PSA_REAL_SDK_LATE_COMPLETIONS_REJECTED);
    TI::Instance().resetTestValue(TI::TestValue::PSA_REAL_SDK_REQUESTS_STARTED);
}

TEST_F(NOINTERACTIVENotLoggedTest, Folderlogin)
{
    {
        auto rLoging = executeInClient({"login", LINK_TESTEXPORTFOLDER});
        ASSERT_TRUE(rLoging.ok());
    }

    {
        auto rLogout = executeInClient({"logout", "--keep-session"});
        ASSERT_TRUE(rLogout.ok());
    }

    {
        EventTracker evTracker({TestInstruments::Event::FETCH_NODES_REQ_UPDATE});
        auto rLoging = executeInClient({"login", "--resume", LINK_TESTEXPORTFOLDER});
        ASSERT_TRUE(rLoging.ok());
        ASSERT_STREQ(rLoging.out().c_str(), "");
        ASSERT_STREQ(rLoging.err().c_str(), "");
        ASSERT_FALSE(evTracker.eventHappened(TestInstruments::Event::FETCH_NODES_REQ_UPDATE));
    }
}

TEST_F(NOINTERACTIVEReadTest, Find)
{
    auto r = executeInClient({"find"});
    ASSERT_TRUE(r.ok());

    std::vector<std::string> result_paths = splitByNewline(r.out());
    ASSERT_THAT(result_paths, testing::Not(testing::IsEmpty()));

    EXPECT_THAT(result_paths, testing::Contains("."));
    EXPECT_THAT(result_paths, testing::Contains("testReadingFolder01"));
    EXPECT_THAT(result_paths, testing::Contains("testReadingFolder01/file03.txt"));
    EXPECT_THAT(result_paths, testing::Contains("testReadingFolder01/folder01/file03.txt"));
    EXPECT_THAT(result_paths, testing::Contains("testReadingFolder01/folder02/subfolder03/file02.txt"));
}

TEST_F(NOINTERACTIVELoggedInTest, Whoami)
{

    {
        G_SUBTEST << "Basic";

        auto r = executeInClient({"whoami"});
        ASSERT_TRUE(r.ok());

        auto out = r.out();
        ASSERT_THAT(out, testing::Not(testing::IsEmpty()));
        ASSERT_THAT(getenv("MEGACMD_TEST_USER"), testing::NotNull());
        EXPECT_EQ(out, std::string("Account e-mail: ").append(getenv("MEGACMD_TEST_USER")).append("\n"));
    }

    {
        G_SUBTEST << "Extended";

        {
           // Ensure there's at least a file around contributing to ROOT usage:
           auto result = executeInClient({"import", LINK_TESTEXPORTFILE01TXT});
           ASSERT_TRUE(result.ok()) << "could not import testExportFile01.txt";
        }

        auto r = executeInClient({"whoami", "-l"});
        ASSERT_TRUE(r.ok());

        std::vector<std::string> details_out = splitByNewline(r.out());

        ASSERT_THAT(details_out, testing::Not(testing::IsEmpty()));
        ASSERT_THAT(details_out, testing::Contains(testing::HasSubstr("Available storage:")));
        ASSERT_THAT(details_out, testing::Contains(testing::HasSubstr("Pro level:")));
        ASSERT_THAT(details_out, testing::Contains(testing::HasSubstr("Current Session")));
        ASSERT_THAT(details_out, testing::Contains(testing::HasSubstr("In RUBBISH:")));

        EXPECT_THAT(details_out, testing::Not(testing::Contains(ContainsStdRegex("Available storage:\\s+0\\.00\\s+Bytes"))));
        EXPECT_THAT(details_out, testing::Not(testing::Contains(ContainsStdRegex("In ROOT:\\s+0\\.00\\s+Bytes"))));
    }
}

TEST_F(NOINTERACTIVEBasicTest, EchoInvalidUtf8)
{
    const std::string validUtf8 = u8"\uc548\uc548\ub155\ud558\uc138\uc694\uc138\uacc4";

#ifdef _WIN32
    // Required because windows converts the args to UTF-16 before sending them to the server,
    // so we can't pass an invalid UTF-8 string all the way to the executer; we need to escape it
    const std::string invalidUtf8 = "<win-invalid-utf8>";
#else
    const std::string invalidUtf8 = "\xf0\x8f\xbf\xbf";
#endif

    auto result = executeInClient({"echo", validUtf8});
    ASSERT_TRUE(result.ok());
    EXPECT_THAT(result.out(), testing::HasSubstr(validUtf8));

    result = executeInClient({"echo", invalidUtf8});
    ASSERT_TRUE(result.ok());
    EXPECT_THAT(result.out(), testing::Not(testing::HasSubstr(invalidUtf8)));

    result = executeInClient({"echo", "--log-as-err", validUtf8});
    ASSERT_TRUE(result.ok());
    EXPECT_THAT(result.err(), testing::HasSubstr(validUtf8));

    result = executeInClient({"echo", "--log-as-err", invalidUtf8});
    ASSERT_TRUE(result.ok());
    EXPECT_THAT(result.err(), testing::HasSubstr("<invalid utf8>"));
    EXPECT_THAT(result.err(), testing::Not(testing::HasSubstr(invalidUtf8)));
    EXPECT_THAT(result.out(), testing::Not(testing::HasSubstr(invalidUtf8)));
}
