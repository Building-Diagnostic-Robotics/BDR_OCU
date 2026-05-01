/****************************************************************************
** Meta object code from reading C++ file 'startup_screen.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../cpp/include/startup_screen.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'startup_screen.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__StartupScreen_t {
    QByteArrayData data[19];
    char stringdata0[348];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__StartupScreen_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__StartupScreen_t qt_meta_stringdata_f2c_cpp__StartupScreen = {
    {
QT_MOC_LITERAL(0, 0, 22), // "f2c_cpp::StartupScreen"
QT_MOC_LITERAL(1, 23, 13), // "backRequested"
QT_MOC_LITERAL(2, 37, 0), // ""
QT_MOC_LITERAL(3, 38, 17), // "continueRequested"
QT_MOC_LITERAL(4, 56, 25), // "onStartDiagnosticsClicked"
QT_MOC_LITERAL(5, 82, 25), // "onRerunDiagnosticsClicked"
QT_MOC_LITERAL(6, 108, 24), // "onLaunchDashboardClicked"
QT_MOC_LITERAL(7, 133, 25), // "onRetryFetchReportClicked"
QT_MOC_LITERAL(8, 159, 22), // "onPreflightStdoutReady"
QT_MOC_LITERAL(9, 182, 22), // "onPreflightStderrReady"
QT_MOC_LITERAL(10, 205, 19), // "onPreflightFinished"
QT_MOC_LITERAL(11, 225, 8), // "exitCode"
QT_MOC_LITERAL(12, 234, 20), // "QProcess::ExitStatus"
QT_MOC_LITERAL(13, 255, 10), // "exitStatus"
QT_MOC_LITERAL(14, 266, 16), // "onPreflightError"
QT_MOC_LITERAL(15, 283, 22), // "QProcess::ProcessError"
QT_MOC_LITERAL(16, 306, 5), // "error"
QT_MOC_LITERAL(17, 312, 21), // "onReportFetchFinished"
QT_MOC_LITERAL(18, 334, 13) // "onReportError"

    },
    "f2c_cpp::StartupScreen\0backRequested\0"
    "\0continueRequested\0onStartDiagnosticsClicked\0"
    "onRerunDiagnosticsClicked\0"
    "onLaunchDashboardClicked\0"
    "onRetryFetchReportClicked\0"
    "onPreflightStdoutReady\0onPreflightStderrReady\0"
    "onPreflightFinished\0exitCode\0"
    "QProcess::ExitStatus\0exitStatus\0"
    "onPreflightError\0QProcess::ProcessError\0"
    "error\0onReportFetchFinished\0onReportError"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__StartupScreen[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      12,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   74,    2, 0x06 /* Public */,
       3,    0,   75,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       4,    0,   76,    2, 0x08 /* Private */,
       5,    0,   77,    2, 0x08 /* Private */,
       6,    0,   78,    2, 0x08 /* Private */,
       7,    0,   79,    2, 0x08 /* Private */,
       8,    0,   80,    2, 0x08 /* Private */,
       9,    0,   81,    2, 0x08 /* Private */,
      10,    2,   82,    2, 0x08 /* Private */,
      14,    1,   87,    2, 0x08 /* Private */,
      17,    2,   90,    2, 0x08 /* Private */,
      18,    1,   95,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 12,   11,   13,
    QMetaType::Void, 0x80000000 | 15,   16,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 12,   11,   13,
    QMetaType::Void, 0x80000000 | 15,   16,

       0        // eod
};

void f2c_cpp::StartupScreen::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<StartupScreen *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->backRequested(); break;
        case 1: _t->continueRequested(); break;
        case 2: _t->onStartDiagnosticsClicked(); break;
        case 3: _t->onRerunDiagnosticsClicked(); break;
        case 4: _t->onLaunchDashboardClicked(); break;
        case 5: _t->onRetryFetchReportClicked(); break;
        case 6: _t->onPreflightStdoutReady(); break;
        case 7: _t->onPreflightStderrReady(); break;
        case 8: _t->onPreflightFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< QProcess::ExitStatus(*)>(_a[2]))); break;
        case 9: _t->onPreflightError((*reinterpret_cast< QProcess::ProcessError(*)>(_a[1]))); break;
        case 10: _t->onReportFetchFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< QProcess::ExitStatus(*)>(_a[2]))); break;
        case 11: _t->onReportError((*reinterpret_cast< QProcess::ProcessError(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (StartupScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&StartupScreen::backRequested)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (StartupScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&StartupScreen::continueRequested)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::StartupScreen::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__StartupScreen.data,
    qt_meta_data_f2c_cpp__StartupScreen,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::StartupScreen::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::StartupScreen::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__StartupScreen.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int f2c_cpp::StartupScreen::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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

// SIGNAL 0
void f2c_cpp::StartupScreen::backRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void f2c_cpp::StartupScreen::continueRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
