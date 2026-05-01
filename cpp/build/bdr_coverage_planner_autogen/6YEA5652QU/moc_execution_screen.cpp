/****************************************************************************
** Meta object code from reading C++ file 'execution_screen.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/execution_screen.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'execution_screen.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__ExecutionScreen_t {
    QByteArrayData data[11];
    char stringdata0[184];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__ExecutionScreen_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__ExecutionScreen_t qt_meta_stringdata_f2c_cpp__ExecutionScreen = {
    {
QT_MOC_LITERAL(0, 0, 24), // "f2c_cpp::ExecutionScreen"
QT_MOC_LITERAL(1, 25, 17), // "executionComplete"
QT_MOC_LITERAL(2, 43, 0), // ""
QT_MOC_LITERAL(3, 44, 13), // "backRequested"
QT_MOC_LITERAL(4, 58, 11), // "scanAborted"
QT_MOC_LITERAL(5, 70, 13), // "onBackClicked"
QT_MOC_LITERAL(6, 84, 19), // "onStartStackClicked"
QT_MOC_LITERAL(7, 104, 25), // "onPublishWaypointsClicked"
QT_MOC_LITERAL(8, 130, 24), // "onStartNavigationClicked"
QT_MOC_LITERAL(9, 155, 14), // "onAbortClicked"
QT_MOC_LITERAL(10, 170, 13) // "onDoneClicked"

    },
    "f2c_cpp::ExecutionScreen\0executionComplete\0"
    "\0backRequested\0scanAborted\0onBackClicked\0"
    "onStartStackClicked\0onPublishWaypointsClicked\0"
    "onStartNavigationClicked\0onAbortClicked\0"
    "onDoneClicked"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__ExecutionScreen[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       9,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       3,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   59,    2, 0x06 /* Public */,
       3,    0,   60,    2, 0x06 /* Public */,
       4,    0,   61,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       5,    0,   62,    2, 0x08 /* Private */,
       6,    0,   63,    2, 0x08 /* Private */,
       7,    0,   64,    2, 0x08 /* Private */,
       8,    0,   65,    2, 0x08 /* Private */,
       9,    0,   66,    2, 0x08 /* Private */,
      10,    0,   67,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::ExecutionScreen::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ExecutionScreen *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->executionComplete(); break;
        case 1: _t->backRequested(); break;
        case 2: _t->scanAborted(); break;
        case 3: _t->onBackClicked(); break;
        case 4: _t->onStartStackClicked(); break;
        case 5: _t->onPublishWaypointsClicked(); break;
        case 6: _t->onStartNavigationClicked(); break;
        case 7: _t->onAbortClicked(); break;
        case 8: _t->onDoneClicked(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (ExecutionScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExecutionScreen::executionComplete)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (ExecutionScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExecutionScreen::backRequested)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (ExecutionScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExecutionScreen::scanAborted)) {
                *result = 2;
                return;
            }
        }
    }
    (void)_a;
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::ExecutionScreen::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__ExecutionScreen.data,
    qt_meta_data_f2c_cpp__ExecutionScreen,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::ExecutionScreen::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::ExecutionScreen::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__ExecutionScreen.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int f2c_cpp::ExecutionScreen::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::ExecutionScreen::executionComplete()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void f2c_cpp::ExecutionScreen::backRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void f2c_cpp::ExecutionScreen::scanAborted()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
