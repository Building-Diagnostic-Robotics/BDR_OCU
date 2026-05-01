/****************************************************************************
** Meta object code from reading C++ file 'launch_manager.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/launch_manager.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'launch_manager.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__LaunchManager_t {
    QByteArrayData data[26];
    char stringdata0[365];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__LaunchManager_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__LaunchManager_t qt_meta_stringdata_f2c_cpp__LaunchManager = {
    {
QT_MOC_LITERAL(0, 0, 22), // "f2c_cpp::LaunchManager"
QT_MOC_LITERAL(1, 23, 19), // "explorationLaunched"
QT_MOC_LITERAL(2, 43, 0), // ""
QT_MOC_LITERAL(3, 44, 18), // "explorationStopped"
QT_MOC_LITERAL(4, 63, 16), // "explorationError"
QT_MOC_LITERAL(5, 80, 3), // "msg"
QT_MOC_LITERAL(6, 84, 8), // "mapSaved"
QT_MOC_LITERAL(7, 93, 10), // "remotePath"
QT_MOC_LITERAL(8, 104, 12), // "mapSaveError"
QT_MOC_LITERAL(9, 117, 11), // "scpFinished"
QT_MOC_LITERAL(10, 129, 9), // "localPath"
QT_MOC_LITERAL(11, 139, 8), // "scpError"
QT_MOC_LITERAL(12, 148, 10), // "logMessage"
QT_MOC_LITERAL(13, 159, 21), // "onLocalLaunchFinished"
QT_MOC_LITERAL(14, 181, 8), // "exitCode"
QT_MOC_LITERAL(15, 190, 20), // "QProcess::ExitStatus"
QT_MOC_LITERAL(16, 211, 6), // "status"
QT_MOC_LITERAL(17, 218, 22), // "onRemoteLaunchFinished"
QT_MOC_LITERAL(18, 241, 18), // "onLocalLaunchError"
QT_MOC_LITERAL(19, 260, 22), // "QProcess::ProcessError"
QT_MOC_LITERAL(20, 283, 3), // "err"
QT_MOC_LITERAL(21, 287, 19), // "onRemoteLaunchError"
QT_MOC_LITERAL(22, 307, 17), // "onSaveMapFinished"
QT_MOC_LITERAL(23, 325, 14), // "onSaveMapError"
QT_MOC_LITERAL(24, 340, 13), // "onScpFinished"
QT_MOC_LITERAL(25, 354, 10) // "onScpError"

    },
    "f2c_cpp::LaunchManager\0explorationLaunched\0"
    "\0explorationStopped\0explorationError\0"
    "msg\0mapSaved\0remotePath\0mapSaveError\0"
    "scpFinished\0localPath\0scpError\0"
    "logMessage\0onLocalLaunchFinished\0"
    "exitCode\0QProcess::ExitStatus\0status\0"
    "onRemoteLaunchFinished\0onLocalLaunchError\0"
    "QProcess::ProcessError\0err\0"
    "onRemoteLaunchError\0onSaveMapFinished\0"
    "onSaveMapError\0onScpFinished\0onScpError"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__LaunchManager[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      16,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       8,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   94,    2, 0x06 /* Public */,
       3,    0,   95,    2, 0x06 /* Public */,
       4,    1,   96,    2, 0x06 /* Public */,
       6,    1,   99,    2, 0x06 /* Public */,
       8,    1,  102,    2, 0x06 /* Public */,
       9,    1,  105,    2, 0x06 /* Public */,
      11,    1,  108,    2, 0x06 /* Public */,
      12,    1,  111,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      13,    2,  114,    2, 0x08 /* Private */,
      17,    2,  119,    2, 0x08 /* Private */,
      18,    1,  124,    2, 0x08 /* Private */,
      21,    1,  127,    2, 0x08 /* Private */,
      22,    2,  130,    2, 0x08 /* Private */,
      23,    1,  135,    2, 0x08 /* Private */,
      24,    2,  138,    2, 0x08 /* Private */,
      25,    1,  143,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    5,
    QMetaType::Void, QMetaType::QString,    7,
    QMetaType::Void, QMetaType::QString,    5,
    QMetaType::Void, QMetaType::QString,   10,
    QMetaType::Void, QMetaType::QString,    5,
    QMetaType::Void, QMetaType::QString,    5,

 // slots: parameters
    QMetaType::Void, QMetaType::Int, 0x80000000 | 15,   14,   16,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 15,   14,   16,
    QMetaType::Void, 0x80000000 | 19,   20,
    QMetaType::Void, 0x80000000 | 19,   20,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 15,   14,   16,
    QMetaType::Void, 0x80000000 | 19,   20,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 15,   14,   16,
    QMetaType::Void, 0x80000000 | 19,   20,

       0        // eod
};

void f2c_cpp::LaunchManager::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<LaunchManager *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->explorationLaunched(); break;
        case 1: _t->explorationStopped(); break;
        case 2: _t->explorationError((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->mapSaved((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: _t->mapSaveError((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 5: _t->scpFinished((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 6: _t->scpError((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 7: _t->logMessage((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 8: _t->onLocalLaunchFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< QProcess::ExitStatus(*)>(_a[2]))); break;
        case 9: _t->onRemoteLaunchFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< QProcess::ExitStatus(*)>(_a[2]))); break;
        case 10: _t->onLocalLaunchError((*reinterpret_cast< QProcess::ProcessError(*)>(_a[1]))); break;
        case 11: _t->onRemoteLaunchError((*reinterpret_cast< QProcess::ProcessError(*)>(_a[1]))); break;
        case 12: _t->onSaveMapFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< QProcess::ExitStatus(*)>(_a[2]))); break;
        case 13: _t->onSaveMapError((*reinterpret_cast< QProcess::ProcessError(*)>(_a[1]))); break;
        case 14: _t->onScpFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< QProcess::ExitStatus(*)>(_a[2]))); break;
        case 15: _t->onScpError((*reinterpret_cast< QProcess::ProcessError(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (LaunchManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&LaunchManager::explorationLaunched)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (LaunchManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&LaunchManager::explorationStopped)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (LaunchManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&LaunchManager::explorationError)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (LaunchManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&LaunchManager::mapSaved)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (LaunchManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&LaunchManager::mapSaveError)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (LaunchManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&LaunchManager::scpFinished)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (LaunchManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&LaunchManager::scpError)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (LaunchManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&LaunchManager::logMessage)) {
                *result = 7;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::LaunchManager::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__LaunchManager.data,
    qt_meta_data_f2c_cpp__LaunchManager,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::LaunchManager::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::LaunchManager::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__LaunchManager.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int f2c_cpp::LaunchManager::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 16)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 16;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 16)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 16;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::LaunchManager::explorationLaunched()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void f2c_cpp::LaunchManager::explorationStopped()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void f2c_cpp::LaunchManager::explorationError(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void f2c_cpp::LaunchManager::mapSaved(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void f2c_cpp::LaunchManager::mapSaveError(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void f2c_cpp::LaunchManager::scpFinished(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void f2c_cpp::LaunchManager::scpError(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void f2c_cpp::LaunchManager::logMessage(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
