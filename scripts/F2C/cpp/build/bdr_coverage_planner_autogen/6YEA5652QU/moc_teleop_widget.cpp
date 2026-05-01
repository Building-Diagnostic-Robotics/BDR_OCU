/****************************************************************************
** Meta object code from reading C++ file 'teleop_widget.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/teleop_widget.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'teleop_widget.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__TeleopWidget_t {
    QByteArrayData data[23];
    char stringdata0[312];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__TeleopWidget_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__TeleopWidget_t qt_meta_stringdata_f2c_cpp__TeleopWidget = {
    {
QT_MOC_LITERAL(0, 0, 21), // "f2c_cpp::TeleopWidget"
QT_MOC_LITERAL(1, 22, 13), // "statusMessage"
QT_MOC_LITERAL(2, 36, 0), // ""
QT_MOC_LITERAL(3, 37, 7), // "message"
QT_MOC_LITERAL(4, 45, 15), // "mpcStateChanged"
QT_MOC_LITERAL(5, 61, 7), // "enabled"
QT_MOC_LITERAL(6, 69, 11), // "onArmMotors"
QT_MOC_LITERAL(7, 81, 14), // "onDisarmMotors"
QT_MOC_LITERAL(8, 96, 11), // "onToggleMpc"
QT_MOC_LITERAL(9, 108, 9), // "onSaveMap"
QT_MOC_LITERAL(10, 118, 16), // "onStartRecording"
QT_MOC_LITERAL(11, 135, 15), // "onStopRecording"
QT_MOC_LITERAL(12, 151, 14), // "onToggleRosbag"
QT_MOC_LITERAL(13, 166, 15), // "onToggleGprScan"
QT_MOC_LITERAL(14, 182, 11), // "onGprLineUp"
QT_MOC_LITERAL(15, 194, 13), // "onGprLineDown"
QT_MOC_LITERAL(16, 208, 20), // "onLinearSpeedChanged"
QT_MOC_LITERAL(17, 229, 5), // "value"
QT_MOC_LITERAL(18, 235, 21), // "onAngularSpeedChanged"
QT_MOC_LITERAL(19, 257, 15), // "onTeleopToggled"
QT_MOC_LITERAL(20, 273, 7), // "checked"
QT_MOC_LITERAL(21, 281, 13), // "publishCmdVel"
QT_MOC_LITERAL(22, 295, 16) // "updateKeyDisplay"

    },
    "f2c_cpp::TeleopWidget\0statusMessage\0"
    "\0message\0mpcStateChanged\0enabled\0"
    "onArmMotors\0onDisarmMotors\0onToggleMpc\0"
    "onSaveMap\0onStartRecording\0onStopRecording\0"
    "onToggleRosbag\0onToggleGprScan\0"
    "onGprLineUp\0onGprLineDown\0"
    "onLinearSpeedChanged\0value\0"
    "onAngularSpeedChanged\0onTeleopToggled\0"
    "checked\0publishCmdVel\0updateKeyDisplay"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__TeleopWidget[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      17,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   99,    2, 0x06 /* Public */,
       4,    1,  102,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       6,    0,  105,    2, 0x08 /* Private */,
       7,    0,  106,    2, 0x08 /* Private */,
       8,    0,  107,    2, 0x08 /* Private */,
       9,    0,  108,    2, 0x08 /* Private */,
      10,    0,  109,    2, 0x08 /* Private */,
      11,    0,  110,    2, 0x08 /* Private */,
      12,    0,  111,    2, 0x08 /* Private */,
      13,    0,  112,    2, 0x08 /* Private */,
      14,    0,  113,    2, 0x08 /* Private */,
      15,    0,  114,    2, 0x08 /* Private */,
      16,    1,  115,    2, 0x08 /* Private */,
      18,    1,  118,    2, 0x08 /* Private */,
      19,    1,  121,    2, 0x08 /* Private */,
      21,    0,  124,    2, 0x08 /* Private */,
      22,    0,  125,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::Bool,    5,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,   17,
    QMetaType::Void, QMetaType::Int,   17,
    QMetaType::Void, QMetaType::Bool,   20,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::TeleopWidget::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<TeleopWidget *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->statusMessage((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->mpcStateChanged((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 2: _t->onArmMotors(); break;
        case 3: _t->onDisarmMotors(); break;
        case 4: _t->onToggleMpc(); break;
        case 5: _t->onSaveMap(); break;
        case 6: _t->onStartRecording(); break;
        case 7: _t->onStopRecording(); break;
        case 8: _t->onToggleRosbag(); break;
        case 9: _t->onToggleGprScan(); break;
        case 10: _t->onGprLineUp(); break;
        case 11: _t->onGprLineDown(); break;
        case 12: _t->onLinearSpeedChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 13: _t->onAngularSpeedChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 14: _t->onTeleopToggled((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 15: _t->publishCmdVel(); break;
        case 16: _t->updateKeyDisplay(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (TeleopWidget::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TeleopWidget::statusMessage)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (TeleopWidget::*)(bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TeleopWidget::mpcStateChanged)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::TeleopWidget::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__TeleopWidget.data,
    qt_meta_data_f2c_cpp__TeleopWidget,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::TeleopWidget::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::TeleopWidget::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__TeleopWidget.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int f2c_cpp::TeleopWidget::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 17)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 17;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 17)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 17;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::TeleopWidget::statusMessage(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void f2c_cpp::TeleopWidget::mpcStateChanged(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
struct qt_meta_stringdata_f2c_cpp__TeleopDockWidget_t {
    QByteArrayData data[1];
    char stringdata0[26];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__TeleopDockWidget_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__TeleopDockWidget_t qt_meta_stringdata_f2c_cpp__TeleopDockWidget = {
    {
QT_MOC_LITERAL(0, 0, 25) // "f2c_cpp::TeleopDockWidget"

    },
    "f2c_cpp::TeleopDockWidget"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__TeleopDockWidget[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       0,    0, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

       0        // eod
};

void f2c_cpp::TeleopDockWidget::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    (void)_o;
    (void)_id;
    (void)_c;
    (void)_a;
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::TeleopDockWidget::staticMetaObject = { {
    QMetaObject::SuperData::link<QDockWidget::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__TeleopDockWidget.data,
    qt_meta_data_f2c_cpp__TeleopDockWidget,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::TeleopDockWidget::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::TeleopDockWidget::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__TeleopDockWidget.stringdata0))
        return static_cast<void*>(this);
    return QDockWidget::qt_metacast(_clname);
}

int f2c_cpp::TeleopDockWidget::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDockWidget::qt_metacall(_c, _id, _a);
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
