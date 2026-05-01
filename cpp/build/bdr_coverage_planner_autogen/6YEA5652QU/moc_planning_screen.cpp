/****************************************************************************
** Meta object code from reading C++ file 'planning_screen.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/planning_screen.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'planning_screen.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__PlanningScreen_t {
    QByteArrayData data[21];
    char stringdata0[303];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__PlanningScreen_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__PlanningScreen_t qt_meta_stringdata_f2c_cpp__PlanningScreen = {
    {
QT_MOC_LITERAL(0, 0, 23), // "f2c_cpp::PlanningScreen"
QT_MOC_LITERAL(1, 24, 16), // "planningComplete"
QT_MOC_LITERAL(2, 41, 0), // ""
QT_MOC_LITERAL(3, 42, 13), // "backRequested"
QT_MOC_LITERAL(4, 56, 13), // "onBackClicked"
QT_MOC_LITERAL(5, 70, 14), // "onStep1Process"
QT_MOC_LITERAL(6, 85, 11), // "onStep1Next"
QT_MOC_LITERAL(7, 97, 15), // "onStep2Generate"
QT_MOC_LITERAL(8, 113, 11), // "onStep2Next"
QT_MOC_LITERAL(9, 125, 19), // "onStep3MakeSegments"
QT_MOC_LITERAL(10, 145, 14), // "onStep3Proceed"
QT_MOC_LITERAL(11, 160, 13), // "onROISelected"
QT_MOC_LITERAL(12, 174, 9), // "Polygon2D"
QT_MOC_LITERAL(13, 184, 3), // "roi"
QT_MOC_LITERAL(14, 188, 18), // "onObstacleSelected"
QT_MOC_LITERAL(15, 207, 8), // "obstacle"
QT_MOC_LITERAL(16, 216, 20), // "onSelectionCancelled"
QT_MOC_LITERAL(17, 237, 20), // "onRectangleCompleted"
QT_MOC_LITERAL(18, 258, 4), // "rect"
QT_MOC_LITERAL(19, 263, 18), // "onPointCloudLoaded"
QT_MOC_LITERAL(20, 282, 20) // "onAutoDetectFinished"

    },
    "f2c_cpp::PlanningScreen\0planningComplete\0"
    "\0backRequested\0onBackClicked\0"
    "onStep1Process\0onStep1Next\0onStep2Generate\0"
    "onStep2Next\0onStep3MakeSegments\0"
    "onStep3Proceed\0onROISelected\0Polygon2D\0"
    "roi\0onObstacleSelected\0obstacle\0"
    "onSelectionCancelled\0onRectangleCompleted\0"
    "rect\0onPointCloudLoaded\0onAutoDetectFinished"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__PlanningScreen[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      15,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   89,    2, 0x06 /* Public */,
       3,    0,   90,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       4,    0,   91,    2, 0x08 /* Private */,
       5,    0,   92,    2, 0x08 /* Private */,
       6,    0,   93,    2, 0x08 /* Private */,
       7,    0,   94,    2, 0x08 /* Private */,
       8,    0,   95,    2, 0x08 /* Private */,
       9,    0,   96,    2, 0x08 /* Private */,
      10,    0,   97,    2, 0x08 /* Private */,
      11,    1,   98,    2, 0x08 /* Private */,
      14,    1,  101,    2, 0x08 /* Private */,
      16,    0,  104,    2, 0x08 /* Private */,
      17,    1,  105,    2, 0x08 /* Private */,
      19,    0,  108,    2, 0x08 /* Private */,
      20,    0,  109,    2, 0x08 /* Private */,

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
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 12,   13,
    QMetaType::Void, 0x80000000 | 12,   15,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 12,   18,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::PlanningScreen::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<PlanningScreen *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->planningComplete(); break;
        case 1: _t->backRequested(); break;
        case 2: _t->onBackClicked(); break;
        case 3: _t->onStep1Process(); break;
        case 4: _t->onStep1Next(); break;
        case 5: _t->onStep2Generate(); break;
        case 6: _t->onStep2Next(); break;
        case 7: _t->onStep3MakeSegments(); break;
        case 8: _t->onStep3Proceed(); break;
        case 9: _t->onROISelected((*reinterpret_cast< const Polygon2D(*)>(_a[1]))); break;
        case 10: _t->onObstacleSelected((*reinterpret_cast< const Polygon2D(*)>(_a[1]))); break;
        case 11: _t->onSelectionCancelled(); break;
        case 12: _t->onRectangleCompleted((*reinterpret_cast< const Polygon2D(*)>(_a[1]))); break;
        case 13: _t->onPointCloudLoaded(); break;
        case 14: _t->onAutoDetectFinished(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (PlanningScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlanningScreen::planningComplete)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (PlanningScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlanningScreen::backRequested)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::PlanningScreen::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__PlanningScreen.data,
    qt_meta_data_f2c_cpp__PlanningScreen,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::PlanningScreen::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::PlanningScreen::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__PlanningScreen.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int f2c_cpp::PlanningScreen::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 15)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 15;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 15)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 15;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::PlanningScreen::planningComplete()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void f2c_cpp::PlanningScreen::backRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
