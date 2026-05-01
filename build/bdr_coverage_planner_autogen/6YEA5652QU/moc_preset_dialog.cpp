/****************************************************************************
** Meta object code from reading C++ file 'preset_dialog.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../cpp/include/preset_dialog.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'preset_dialog.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__PresetManagerDialog_t {
    QByteArrayData data[16];
    char stringdata0[198];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__PresetManagerDialog_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__PresetManagerDialog_t qt_meta_stringdata_f2c_cpp__PresetManagerDialog = {
    {
QT_MOC_LITERAL(0, 0, 28), // "f2c_cpp::PresetManagerDialog"
QT_MOC_LITERAL(1, 29, 14), // "presetSelected"
QT_MOC_LITERAL(2, 44, 0), // ""
QT_MOC_LITERAL(3, 45, 4), // "name"
QT_MOC_LITERAL(4, 50, 19), // "presetLoadRequested"
QT_MOC_LITERAL(5, 70, 18), // "onSelectionChanged"
QT_MOC_LITERAL(6, 89, 19), // "onItemDoubleClicked"
QT_MOC_LITERAL(7, 109, 16), // "QListWidgetItem*"
QT_MOC_LITERAL(8, 126, 4), // "item"
QT_MOC_LITERAL(9, 131, 8), // "onRename"
QT_MOC_LITERAL(10, 140, 11), // "onDuplicate"
QT_MOC_LITERAL(11, 152, 8), // "onDelete"
QT_MOC_LITERAL(12, 161, 8), // "onImport"
QT_MOC_LITERAL(13, 170, 8), // "onExport"
QT_MOC_LITERAL(14, 179, 6), // "onLoad"
QT_MOC_LITERAL(15, 186, 11) // "refreshList"

    },
    "f2c_cpp::PresetManagerDialog\0"
    "presetSelected\0\0name\0presetLoadRequested\0"
    "onSelectionChanged\0onItemDoubleClicked\0"
    "QListWidgetItem*\0item\0onRename\0"
    "onDuplicate\0onDelete\0onImport\0onExport\0"
    "onLoad\0refreshList"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__PresetManagerDialog[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      11,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   69,    2, 0x06 /* Public */,
       4,    1,   72,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       5,    0,   75,    2, 0x08 /* Private */,
       6,    1,   76,    2, 0x08 /* Private */,
       9,    0,   79,    2, 0x08 /* Private */,
      10,    0,   80,    2, 0x08 /* Private */,
      11,    0,   81,    2, 0x08 /* Private */,
      12,    0,   82,    2, 0x08 /* Private */,
      13,    0,   83,    2, 0x08 /* Private */,
      14,    0,   84,    2, 0x08 /* Private */,
      15,    0,   85,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QString,    3,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 7,    8,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::PresetManagerDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<PresetManagerDialog *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->presetSelected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->presetLoadRequested((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->onSelectionChanged(); break;
        case 3: _t->onItemDoubleClicked((*reinterpret_cast< QListWidgetItem*(*)>(_a[1]))); break;
        case 4: _t->onRename(); break;
        case 5: _t->onDuplicate(); break;
        case 6: _t->onDelete(); break;
        case 7: _t->onImport(); break;
        case 8: _t->onExport(); break;
        case 9: _t->onLoad(); break;
        case 10: _t->refreshList(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (PresetManagerDialog::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PresetManagerDialog::presetSelected)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (PresetManagerDialog::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PresetManagerDialog::presetLoadRequested)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::PresetManagerDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__PresetManagerDialog.data,
    qt_meta_data_f2c_cpp__PresetManagerDialog,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::PresetManagerDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::PresetManagerDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__PresetManagerDialog.stringdata0))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int f2c_cpp::PresetManagerDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDialog::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 11)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 11;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 11)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 11;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::PresetManagerDialog::presetSelected(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void f2c_cpp::PresetManagerDialog::presetLoadRequested(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
struct qt_meta_stringdata_f2c_cpp__NewPresetDialog_t {
    QByteArrayData data[3];
    char stringdata0[44];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__NewPresetDialog_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__NewPresetDialog_t qt_meta_stringdata_f2c_cpp__NewPresetDialog = {
    {
QT_MOC_LITERAL(0, 0, 24), // "f2c_cpp::NewPresetDialog"
QT_MOC_LITERAL(1, 25, 17), // "validateAndAccept"
QT_MOC_LITERAL(2, 43, 0) // ""

    },
    "f2c_cpp::NewPresetDialog\0validateAndAccept\0"
    ""
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__NewPresetDialog[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       1,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    0,   19,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::NewPresetDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<NewPresetDialog *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->validateAndAccept(); break;
        default: ;
        }
    }
    (void)_a;
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::NewPresetDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__NewPresetDialog.data,
    qt_meta_data_f2c_cpp__NewPresetDialog,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::NewPresetDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::NewPresetDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__NewPresetDialog.stringdata0))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int f2c_cpp::NewPresetDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDialog::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 1)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 1;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 1)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 1;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
