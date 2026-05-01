/****************************************************************************
** Meta object code from reading C++ file 'network_monitor.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/network_monitor.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'network_monitor.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__NetworkMonitor_t {
    QByteArrayData data[13];
    char stringdata0[184];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__NetworkMonitor_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__NetworkMonitor_t qt_meta_stringdata_f2c_cpp__NetworkMonitor = {
    {
QT_MOC_LITERAL(0, 0, 23), // "f2c_cpp::NetworkMonitor"
QT_MOC_LITERAL(1, 24, 19), // "connectivityChanged"
QT_MOC_LITERAL(2, 44, 0), // ""
QT_MOC_LITERAL(3, 45, 6), // "online"
QT_MOC_LITERAL(4, 52, 20), // "networkStatusUpdated"
QT_MOC_LITERAL(5, 73, 13), // "NetworkStatus"
QT_MOC_LITERAL(6, 87, 6), // "status"
QT_MOC_LITERAL(7, 94, 21), // "awsValidationComplete"
QT_MOC_LITERAL(8, 116, 9), // "AwsStatus"
QT_MOC_LITERAL(9, 126, 14), // "onPingFinished"
QT_MOC_LITERAL(10, 141, 8), // "exitCode"
QT_MOC_LITERAL(11, 150, 20), // "QProcess::ExitStatus"
QT_MOC_LITERAL(12, 171, 12) // "performCheck"

    },
    "f2c_cpp::NetworkMonitor\0connectivityChanged\0"
    "\0online\0networkStatusUpdated\0NetworkStatus\0"
    "status\0awsValidationComplete\0AwsStatus\0"
    "onPingFinished\0exitCode\0QProcess::ExitStatus\0"
    "performCheck"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__NetworkMonitor[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       3,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   39,    2, 0x06 /* Public */,
       4,    1,   42,    2, 0x06 /* Public */,
       7,    1,   45,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       9,    2,   48,    2, 0x08 /* Private */,
      12,    0,   53,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::Bool,    3,
    QMetaType::Void, 0x80000000 | 5,    6,
    QMetaType::Void, 0x80000000 | 8,    6,

 // slots: parameters
    QMetaType::Void, QMetaType::Int, 0x80000000 | 11,   10,    6,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::NetworkMonitor::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<NetworkMonitor *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->connectivityChanged((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 1: _t->networkStatusUpdated((*reinterpret_cast< const NetworkStatus(*)>(_a[1]))); break;
        case 2: _t->awsValidationComplete((*reinterpret_cast< const AwsStatus(*)>(_a[1]))); break;
        case 3: _t->onPingFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< QProcess::ExitStatus(*)>(_a[2]))); break;
        case 4: _t->performCheck(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (NetworkMonitor::*)(bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&NetworkMonitor::connectivityChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (NetworkMonitor::*)(const NetworkStatus & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&NetworkMonitor::networkStatusUpdated)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (NetworkMonitor::*)(const AwsStatus & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&NetworkMonitor::awsValidationComplete)) {
                *result = 2;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::NetworkMonitor::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__NetworkMonitor.data,
    qt_meta_data_f2c_cpp__NetworkMonitor,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::NetworkMonitor::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::NetworkMonitor::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__NetworkMonitor.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int f2c_cpp::NetworkMonitor::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::NetworkMonitor::connectivityChanged(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void f2c_cpp::NetworkMonitor::networkStatusUpdated(const NetworkStatus & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void f2c_cpp::NetworkMonitor::awsValidationComplete(const AwsStatus & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
