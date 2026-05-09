/****************************************************************************
** Meta object code from reading C++ file 'update_checker.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/update/update_checker.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'update_checker.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__update__UpdateChecker_t {
    QByteArrayData data[11];
    char stringdata0[165];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__update__UpdateChecker_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__update__UpdateChecker_t qt_meta_stringdata_f2c_cpp__update__UpdateChecker = {
    {
QT_MOC_LITERAL(0, 0, 30), // "f2c_cpp::update::UpdateChecker"
QT_MOC_LITERAL(1, 31, 15), // "updateAvailable"
QT_MOC_LITERAL(2, 47, 0), // ""
QT_MOC_LITERAL(3, 48, 28), // "f2c_cpp::update::VersionInfo"
QT_MOC_LITERAL(4, 77, 4), // "info"
QT_MOC_LITERAL(5, 82, 17), // "noUpdateAvailable"
QT_MOC_LITERAL(6, 100, 11), // "checkFailed"
QT_MOC_LITERAL(7, 112, 6), // "reason"
QT_MOC_LITERAL(8, 119, 12), // "performCheck"
QT_MOC_LITERAL(9, 132, 15), // "onReplyFinished"
QT_MOC_LITERAL(10, 148, 16) // "onRequestTimeout"

    },
    "f2c_cpp::update::UpdateChecker\0"
    "updateAvailable\0\0f2c_cpp::update::VersionInfo\0"
    "info\0noUpdateAvailable\0checkFailed\0"
    "reason\0performCheck\0onReplyFinished\0"
    "onRequestTimeout"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__update__UpdateChecker[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       6,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       3,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   44,    2, 0x06 /* Public */,
       5,    0,   47,    2, 0x06 /* Public */,
       6,    1,   48,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       8,    0,   51,    2, 0x08 /* Private */,
       9,    0,   52,    2, 0x08 /* Private */,
      10,    0,   53,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    7,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::update::UpdateChecker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<UpdateChecker *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->updateAvailable((*reinterpret_cast< const f2c_cpp::update::VersionInfo(*)>(_a[1]))); break;
        case 1: _t->noUpdateAvailable(); break;
        case 2: _t->checkFailed((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->performCheck(); break;
        case 4: _t->onReplyFinished(); break;
        case 5: _t->onRequestTimeout(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (UpdateChecker::*)(const f2c_cpp::update::VersionInfo & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateChecker::updateAvailable)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (UpdateChecker::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateChecker::noUpdateAvailable)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (UpdateChecker::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateChecker::checkFailed)) {
                *result = 2;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::update::UpdateChecker::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__update__UpdateChecker.data,
    qt_meta_data_f2c_cpp__update__UpdateChecker,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::update::UpdateChecker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::update::UpdateChecker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__update__UpdateChecker.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int f2c_cpp::update::UpdateChecker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::update::UpdateChecker::updateAvailable(const f2c_cpp::update::VersionInfo & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void f2c_cpp::update::UpdateChecker::noUpdateAvailable()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void f2c_cpp::update::UpdateChecker::checkFailed(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
