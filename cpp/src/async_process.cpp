#include "async_process.hpp"

#include <QTimer>

#include <memory>

namespace f2c_cpp {
namespace async_proc {

namespace {
/** Grace between escalation rungs: SIGTERM -> SIGKILL -> give up waiting. */
constexpr int kEscalationStepMs = 1000;
}  // namespace

QProcess* run(QObject* parent, const QString& program, const QStringList& args,
              int timeout_ms, Finished on_done) {
    auto* proc = new QProcess(parent);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    // One shared latch. `finished` and `errorOccurred` can both fire, and
    // the deadline can fire while either is in flight; the callback must
    // still run exactly once.
    auto fired = std::make_shared<bool>(false);
    const auto settle = [proc, fired, on_done](int code) {
        if (*fired) {
            return;
        }
        *fired = true;
        const QString out =
            QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        proc->deleteLater();
        if (on_done) {
            on_done(code, out);
        }
    };

    QObject::connect(
        proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        proc, [settle](int code, QProcess::ExitStatus) { settle(code); });
    QObject::connect(proc, &QProcess::errorOccurred, proc,
                     [settle](QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart) {
                             settle(-1);
                         }
                     });

    if (timeout_ms > 0) {
        QTimer::singleShot(timeout_ms, proc, [proc, fired, settle] {
            if (*fired) {
                return;
            }
            proc->terminate();
            QTimer::singleShot(kEscalationStepMs, proc, [proc, fired, settle] {
                if (*fired) {
                    return;
                }
                proc->kill();
                // A killed process reports `finished`; this rung only
                // exists so a chain built on the callback cannot stall.
                QTimer::singleShot(kEscalationStepMs, proc,
                                   [settle] { settle(-1); });
            });
        });
    }

    proc->start(program, args);
    return proc;
}

void reap(QProcess* proc, int grace_ms, std::function<void()> on_done) {
    if (!proc) {
        if (on_done) {
            on_done();
        }
        return;
    }
    if (proc->state() == QProcess::NotRunning) {
        // Still async: callers chain steps from here and must not re-enter
        // themselves inside their own call.
        QTimer::singleShot(0, proc, [on_done] {
            if (on_done) {
                on_done();
            }
        });
        return;
    }

    auto fired = std::make_shared<bool>(false);
    auto conn = std::make_shared<QMetaObject::Connection>();
    const auto settle = [fired, conn, on_done] {
        if (*fired) {
            return;
        }
        *fired = true;
        QObject::disconnect(*conn);
        if (on_done) {
            on_done();
        }
    };
    *conn = QObject::connect(
        proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        proc, [settle](int, QProcess::ExitStatus) { settle(); });

    QTimer::singleShot(grace_ms, proc, [proc, fired, settle] {
        if (*fired) {
            return;
        }
        proc->kill();
        QTimer::singleShot(kEscalationStepMs, proc, settle);
    });
}

}  // namespace async_proc
}  // namespace f2c_cpp
