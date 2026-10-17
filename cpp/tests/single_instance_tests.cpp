/**
 * @file single_instance_tests.cpp
 * @brief One OCU per user. Lock behaviour is checked across real processes,
 *        because that is the only place it means anything.
 */

#include "single_instance.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using f2c_cpp::SingleInstance;

namespace {

/** Fork a child that takes the lock, tells the parent, then runs `then`. */
pid_t forkHolder(const QString& path, const std::function<void()>& then) {
    int ready[2];
    if (::pipe(ready) != 0) {
        return -1;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(ready[0]);
        SingleInstance holder;
        const bool ok = holder.acquire(0, path);
        const char byte = ok ? '1' : '0';
        (void)::write(ready[1], &byte, 1);
        ::close(ready[1]);
        then();
        ::_exit(0);
    }
    ::close(ready[1]);
    char byte = 0;
    (void)::read(ready[0], &byte, 1);
    ::close(ready[0]);
    return byte == '1' ? pid : -1;
}

void reap(pid_t pid) {
    int status = 0;
    ::waitpid(pid, &status, 0);
}

}  // namespace

TEST(SingleInstance, SecondCopyIsRefusedWhileFirstRuns) {
    QTemporaryDir dir;
    const QString path = dir.filePath("ocu_instance.lock");
    const pid_t holder = forkHolder(path, [] { ::sleep(30); });
    ASSERT_GT(holder, 0);

    SingleInstance second;
    EXPECT_FALSE(second.acquire(300, path));

    ::kill(holder, SIGTERM);
    reap(holder);
}

TEST(SingleInstance, FreeOnceTheFirstCopyExits) {
    QTemporaryDir dir;
    const QString path = dir.filePath("ocu_instance.lock");
    const pid_t holder = forkHolder(path, [] {});
    ASSERT_GT(holder, 0);
    reap(holder);

    SingleInstance next;
    EXPECT_TRUE(next.acquire(0, path));
}

TEST(SingleInstance, FreeAfterACrash) {
    // kill -9: no destructor runs; the kernel still drops the flock.
    QTemporaryDir dir;
    const QString path = dir.filePath("ocu_instance.lock");
    const pid_t holder = forkHolder(path, [] { ::sleep(30); });
    ASSERT_GT(holder, 0);
    ::kill(holder, SIGKILL);
    reap(holder);

    SingleInstance next;
    EXPECT_TRUE(next.acquire(0, path));
}

TEST(SingleInstance, ProcessesTheOcuSpawnedDoNotKeepTheLock) {
    // The OCU starts ssh, launch trees and the update runner. The lock fd is
    // O_CLOEXEC, so a child still running after the OCU exits must not
    // hold it.
    QTemporaryDir dir;
    const QString path = dir.filePath("ocu_instance.lock");
    int gc_pipe[2];
    ASSERT_EQ(::pipe(gc_pipe), 0);
    const pid_t holder = forkHolder(path, [gc_pipe] {
        const pid_t grandchild = ::fork();
        if (grandchild == 0) {
            ::execlp("sleep", "sleep", "30", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        (void)::write(gc_pipe[1], &grandchild, sizeof(grandchild));
    });
    ASSERT_GT(holder, 0);
    pid_t grandchild = 0;
    ASSERT_EQ(::read(gc_pipe[0], &grandchild, sizeof(grandchild)),
              ssize_t(sizeof(grandchild)));
    reap(holder);  // the "OCU" is gone; its child is still running
    ASSERT_EQ(::kill(grandchild, 0), 0) << "spawned child should be alive";
    // Between fork and exec the child still shares the fd; O_CLOEXEC drops
    // it at exec. Wait until the child really is `sleep` (a few ms in the
    // app, and the 3 s acquire retry covers that window anyway).
    QElapsedTimer exec_wait;
    exec_wait.start();
    bool execd = false;
    while (!execd && exec_wait.elapsed() < 2000) {
        QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(grandchild));
        execd = cmdline.open(QIODevice::ReadOnly) &&
                cmdline.readAll().startsWith("sleep");
        if (!execd) {
            ::usleep(10 * 1000);
        }
    }
    ASSERT_TRUE(execd) << "spawned child never reached exec";

    SingleInstance next;
    EXPECT_TRUE(next.acquire(0, path));

    ::kill(grandchild, SIGKILL);
    ::waitpid(grandchild, nullptr, 0);
}

TEST(SingleInstance, WaitsForACopyThatIsStillClosing) {
    QTemporaryDir dir;
    const QString path = dir.filePath("ocu_instance.lock");
    const pid_t holder = forkHolder(path, [] { ::usleep(500 * 1000); });
    ASSERT_GT(holder, 0);

    SingleInstance next;
    QElapsedTimer t;
    t.start();
    EXPECT_TRUE(next.acquire(3000, path));
    EXPECT_GE(t.elapsed(), 300);
    reap(holder);
}

TEST(SingleInstance, RaiseRequestReachesTheRunningCopy) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);
    QTemporaryDir dir;
    const QString name =
        QStringLiteral("bdr-ocu-test-%1").arg(QCoreApplication::applicationPid());

    SingleInstance first;
    ASSERT_TRUE(first.acquire(0, dir.filePath("ocu_instance.lock")));
    first.listen(name);
    bool raised = false;
    QObject::connect(&first, &SingleInstance::raiseRequested,
                     [&] { raised = true; });

    std::atomic<int> answered{-1};
    std::thread second([&] {
        answered = SingleInstance::askRunningToRaise(2000, name) ? 1 : 0;
    });
    QElapsedTimer t;
    t.start();
    while (answered.load() < 0 && t.elapsed() < 3000) {
        app.processEvents(QEventLoop::AllEvents, 50);
    }
    second.join();
    EXPECT_EQ(answered.load(), 1);
    EXPECT_TRUE(raised);
}

TEST(SingleInstance, NoRunningCopyMeansNoAnswer) {
    // The fallback message shows in this case.
    EXPECT_FALSE(SingleInstance::askRunningToRaise(
        300, QStringLiteral("bdr-ocu-test-nobody-%1").arg(::getpid())));
}

TEST(SingleInstance, AClosingCopyIsNotRaised) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);
    QTemporaryDir dir;
    const QString name = QStringLiteral("bdr-ocu-test-closing-%1")
                             .arg(QCoreApplication::applicationPid());
    SingleInstance first;
    ASSERT_TRUE(first.acquire(0, dir.filePath("ocu_instance.lock")));
    first.listen(name);
    first.stopListening();  // window accepted close
    EXPECT_FALSE(SingleInstance::askRunningToRaise(300, name));
    EXPECT_TRUE(first.held());  // lock kept until the process exits
}
