/**
 * @file async_process_tests.cpp
 * @brief The contract the launch / teardown chains are built on:
 *        `async_proc::run` and `async_proc::reap` call back exactly once,
 *        on success, on a spawn failure, and on the deadline. A chain step
 *        that never gets its callback strands the operator on a page that
 *        cannot move, which is the exact failure these replaced.
 */

#include "async_process.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

using f2c_cpp::async_proc::reap;
using f2c_cpp::async_proc::run;

namespace {

/** Spins the event loop until `done` or `ceiling_ms`, whichever is first. */
void pump(const bool& done, int ceiling_ms) {
    QElapsedTimer clock;
    clock.start();
    while (!done && clock.elapsed() < ceiling_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

}  // namespace

TEST(AsyncProcess, ReportsExitCodeAndOutputOnce) {
    int calls = 0;
    int code = -99;
    QString output;
    bool done = false;
    run(QCoreApplication::instance(), QStringLiteral("bash"),
        QStringList() << QStringLiteral("-c")
                      << QStringLiteral("echo hello; exit 3"),
        5000, [&](int rc, const QString& out) {
            ++calls;
            code = rc;
            output = out;
            done = true;
        });
    pump(done, 5000);

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(code, 3);
    EXPECT_EQ(output, QStringLiteral("hello"));
}

TEST(AsyncProcess, MissingProgramStillCallsBack) {
    bool done = false;
    int calls = 0;
    int code = 0;
    run(QCoreApplication::instance(),
        QStringLiteral("bdr-no-such-program-exists"), QStringList(), 5000,
        [&](int rc, const QString&) {
            ++calls;
            code = rc;
            done = true;
        });
    pump(done, 5000);

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(code, -1);
}

TEST(AsyncProcess, DeadlineKillsAndCallsBack) {
    bool done = false;
    int calls = 0;
    QElapsedTimer clock;
    clock.start();
    run(QCoreApplication::instance(), QStringLiteral("sleep"),
        QStringList() << QStringLiteral("30"), 200,
        [&](int, const QString&) {
            ++calls;
            done = true;
        });
    pump(done, 8000);

    EXPECT_EQ(calls, 1);
    EXPECT_LT(clock.elapsed(), 4000);
}

TEST(AsyncProcess, ReapOfIdleProcessIsAsyncAndOnce) {
    QProcess idle;
    bool done = false;
    int calls = 0;
    reap(&idle, 1000, [&] {
        ++calls;
        done = true;
    });
    // Deliberately not synchronous: chain steps call reap from inside their
    // own step and must not re-enter themselves.
    EXPECT_EQ(calls, 0);
    pump(done, 2000);
    EXPECT_EQ(calls, 1);
}

TEST(AsyncProcess, ReapKillsAfterGrace) {
    QProcess proc;
    proc.start(QStringLiteral("sleep"), QStringList() << QStringLiteral("30"));
    ASSERT_TRUE(proc.waitForStarted(3000));

    bool done = false;
    int calls = 0;
    reap(&proc, 200, [&] {
        ++calls;
        done = true;
    });
    pump(done, 8000);

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(proc.state(), QProcess::NotRunning);
}

// One QCoreApplication for the whole binary: these helpers are timer-driven,
// and a per-test app would leave every test after the first without an event
// loop to deliver them.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
