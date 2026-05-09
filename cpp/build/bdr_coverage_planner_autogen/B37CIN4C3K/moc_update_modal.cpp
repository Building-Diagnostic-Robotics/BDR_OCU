/****************************************************************************
** Meta object code from reading C++ file 'update_modal.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/components/update_modal.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'update_modal.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__UpdateModal_t {
    QByteArrayData data[9];
    char stringdata0[150];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__UpdateModal_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__UpdateModal_t qt_meta_stringdata_f2c_cpp__UpdateModal = {
    {
QT_MOC_LITERAL(0, 0, 20), // "f2c_cpp::UpdateModal"
QT_MOC_LITERAL(1, 21, 22), // "remindMeLaterRequested"
QT_MOC_LITERAL(2, 44, 0), // ""
QT_MOC_LITERAL(3, 45, 28), // "f2c_cpp::update::VersionInfo"
QT_MOC_LITERAL(4, 74, 4), // "info"
QT_MOC_LITERAL(5, 79, 16), // "installRequested"
QT_MOC_LITERAL(6, 96, 13), // "onPollBattery"
QT_MOC_LITERAL(7, 110, 16), // "onInstallClicked"
QT_MOC_LITERAL(8, 127, 22) // "onRemindMeLaterClicked"

    },
    "f2c_cpp::UpdateModal\0remindMeLaterRequested\0"
    "\0f2c_cpp::update::VersionInfo\0info\0"
    "installRequested\0onPollBattery\0"
    "onInstallClicked\0onRemindMeLaterClicked"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__UpdateModal[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   39,    2, 0x06 /* Public */,
       5,    1,   42,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       6,    0,   45,    2, 0x08 /* Private */,
       7,    0,   46,    2, 0x08 /* Private */,
       8,    0,   47,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, 0x80000000 | 3,    4,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::UpdateModal::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<UpdateModal *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->remindMeLaterRequested((*reinterpret_cast< const f2c_cpp::update::VersionInfo(*)>(_a[1]))); break;
        case 1: _t->installRequested((*reinterpret_cast< const f2c_cpp::update::VersionInfo(*)>(_a[1]))); break;
        case 2: _t->onPollBattery(); break;
        case 3: _t->onInstallClicked(); break;
        case 4: _t->onRemindMeLaterClicked(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (UpdateModal::*)(const f2c_cpp::update::VersionInfo & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateModal::remindMeLaterRequested)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (UpdateModal::*)(const f2c_cpp::update::VersionInfo & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateModal::installRequested)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::UpdateModal::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__UpdateModal.data,
    qt_meta_data_f2c_cpp__UpdateModal,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::UpdateModal::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::UpdateModal::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__UpdateModal.stringdata0))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int f2c_cpp::UpdateModal::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDialog::qt_metacall(_c, _id, _a);
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
void f2c_cpp::UpdateModal::remindMeLaterRequested(const f2c_cpp::update::VersionInfo & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void f2c_cpp::UpdateModal::installRequested(const f2c_cpp::update::VersionInfo & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
