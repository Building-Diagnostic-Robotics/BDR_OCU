/****************************************************************************
** Meta object code from reading C++ file 'update_runner_window.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/runner/update_runner_window.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'update_runner_window.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__UpdateRunnerWindow_t {
    QByteArrayData data[23];
    char stringdata0[336];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__UpdateRunnerWindow_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__UpdateRunnerWindow_t qt_meta_stringdata_f2c_cpp__UpdateRunnerWindow = {
    {
QT_MOC_LITERAL(0, 0, 27), // "f2c_cpp::UpdateRunnerWindow"
QT_MOC_LITERAL(1, 28, 18), // "onDownloadProgress"
QT_MOC_LITERAL(2, 47, 0), // ""
QT_MOC_LITERAL(3, 48, 8), // "received"
QT_MOC_LITERAL(4, 57, 5), // "total"
QT_MOC_LITERAL(5, 63, 18), // "onDownloadComplete"
QT_MOC_LITERAL(6, 82, 7), // "debPath"
QT_MOC_LITERAL(7, 90, 16), // "onDownloadFailed"
QT_MOC_LITERAL(8, 107, 6), // "reason"
QT_MOC_LITERAL(9, 114, 16), // "onRetryScheduled"
QT_MOC_LITERAL(10, 131, 7), // "attempt"
QT_MOC_LITERAL(11, 139, 13), // "totalAttempts"
QT_MOC_LITERAL(12, 153, 7), // "delayMs"
QT_MOC_LITERAL(13, 161, 20), // "onRetryCountdownTick"
QT_MOC_LITERAL(14, 182, 22), // "onRestartCountdownTick"
QT_MOC_LITERAL(15, 205, 17), // "onTryAgainClicked"
QT_MOC_LITERAL(16, 223, 15), // "onCancelClicked"
QT_MOC_LITERAL(17, 239, 20), // "onShowDetailsToggled"
QT_MOC_LITERAL(18, 260, 16), // "onLogFileChanged"
QT_MOC_LITERAL(19, 277, 21), // "onDpkgReadyReadStdout"
QT_MOC_LITERAL(20, 299, 14), // "onDpkgFinished"
QT_MOC_LITERAL(21, 314, 9), // "exit_code"
QT_MOC_LITERAL(22, 324, 11) // "exit_status"

    },
    "f2c_cpp::UpdateRunnerWindow\0"
    "onDownloadProgress\0\0received\0total\0"
    "onDownloadComplete\0debPath\0onDownloadFailed\0"
    "reason\0onRetryScheduled\0attempt\0"
    "totalAttempts\0delayMs\0onRetryCountdownTick\0"
    "onRestartCountdownTick\0onTryAgainClicked\0"
    "onCancelClicked\0onShowDetailsToggled\0"
    "onLogFileChanged\0onDpkgReadyReadStdout\0"
    "onDpkgFinished\0exit_code\0exit_status"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__UpdateRunnerWindow[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      12,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    2,   74,    2, 0x08 /* Private */,
       5,    1,   79,    2, 0x08 /* Private */,
       7,    1,   82,    2, 0x08 /* Private */,
       9,    3,   85,    2, 0x08 /* Private */,
      13,    0,   92,    2, 0x08 /* Private */,
      14,    0,   93,    2, 0x08 /* Private */,
      15,    0,   94,    2, 0x08 /* Private */,
      16,    0,   95,    2, 0x08 /* Private */,
      17,    0,   96,    2, 0x08 /* Private */,
      18,    0,   97,    2, 0x08 /* Private */,
      19,    0,   98,    2, 0x08 /* Private */,
      20,    2,   99,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void, QMetaType::LongLong, QMetaType::LongLong,    3,    4,
    QMetaType::Void, QMetaType::QString,    6,
    QMetaType::Void, QMetaType::QString,    8,
    QMetaType::Void, QMetaType::Int, QMetaType::Int, QMetaType::Int,   10,   11,   12,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,   21,   22,

       0        // eod
};

void f2c_cpp::UpdateRunnerWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<UpdateRunnerWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->onDownloadProgress((*reinterpret_cast< qint64(*)>(_a[1])),(*reinterpret_cast< qint64(*)>(_a[2]))); break;
        case 1: _t->onDownloadComplete((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->onDownloadFailed((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->onRetryScheduled((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2])),(*reinterpret_cast< int(*)>(_a[3]))); break;
        case 4: _t->onRetryCountdownTick(); break;
        case 5: _t->onRestartCountdownTick(); break;
        case 6: _t->onTryAgainClicked(); break;
        case 7: _t->onCancelClicked(); break;
        case 8: _t->onShowDetailsToggled(); break;
        case 9: _t->onLogFileChanged(); break;
        case 10: _t->onDpkgReadyReadStdout(); break;
        case 11: _t->onDpkgFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        default: ;
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::UpdateRunnerWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__UpdateRunnerWindow.data,
    qt_meta_data_f2c_cpp__UpdateRunnerWindow,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::UpdateRunnerWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::UpdateRunnerWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__UpdateRunnerWindow.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int f2c_cpp::UpdateRunnerWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 12)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 12;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 12)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 12;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
