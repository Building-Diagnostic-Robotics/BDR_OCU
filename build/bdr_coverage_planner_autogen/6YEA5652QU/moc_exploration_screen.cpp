/****************************************************************************
** Meta object code from reading C++ file 'exploration_screen.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../cpp/include/exploration_screen.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'exploration_screen.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__ExplorationScreen_t {
    QByteArrayData data[19];
    char stringdata0[358];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__ExplorationScreen_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__ExplorationScreen_t qt_meta_stringdata_f2c_cpp__ExplorationScreen = {
    {
QT_MOC_LITERAL(0, 0, 26), // "f2c_cpp::ExplorationScreen"
QT_MOC_LITERAL(1, 27, 13), // "backRequested"
QT_MOC_LITERAL(2, 41, 0), // ""
QT_MOC_LITERAL(3, 42, 18), // "startScanRequested"
QT_MOC_LITERAL(4, 61, 22), // "finishSaveMapRequested"
QT_MOC_LITERAL(5, 84, 22), // "startPlanningRequested"
QT_MOC_LITERAL(6, 107, 21), // "stopPipelineRequested"
QT_MOC_LITERAL(7, 129, 20), // "teleopTwistRequested"
QT_MOC_LITERAL(8, 150, 8), // "linear_x"
QT_MOC_LITERAL(9, 159, 9), // "angular_z"
QT_MOC_LITERAL(10, 169, 18), // "teleopArmRequested"
QT_MOC_LITERAL(11, 188, 21), // "teleopDisarmRequested"
QT_MOC_LITERAL(12, 210, 26), // "teleopGprPowerOffRequested"
QT_MOC_LITERAL(13, 237, 18), // "onDashboardClicked"
QT_MOC_LITERAL(14, 256, 18), // "onStartScanClicked"
QT_MOC_LITERAL(15, 275, 22), // "onStartPlanningClicked"
QT_MOC_LITERAL(16, 298, 21), // "onStopPipelineClicked"
QT_MOC_LITERAL(17, 320, 17), // "onMappingLockTick"
QT_MOC_LITERAL(18, 338, 19) // "onTeleopPublishTick"

    },
    "f2c_cpp::ExplorationScreen\0backRequested\0"
    "\0startScanRequested\0finishSaveMapRequested\0"
    "startPlanningRequested\0stopPipelineRequested\0"
    "teleopTwistRequested\0linear_x\0angular_z\0"
    "teleopArmRequested\0teleopDisarmRequested\0"
    "teleopGprPowerOffRequested\0"
    "onDashboardClicked\0onStartScanClicked\0"
    "onStartPlanningClicked\0onStopPipelineClicked\0"
    "onMappingLockTick\0onTeleopPublishTick"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__ExplorationScreen[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      15,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       9,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   89,    2, 0x06 /* Public */,
       3,    0,   90,    2, 0x06 /* Public */,
       4,    0,   91,    2, 0x06 /* Public */,
       5,    0,   92,    2, 0x06 /* Public */,
       6,    0,   93,    2, 0x06 /* Public */,
       7,    2,   94,    2, 0x06 /* Public */,
      10,    0,   99,    2, 0x06 /* Public */,
      11,    0,  100,    2, 0x06 /* Public */,
      12,    0,  101,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      13,    0,  102,    2, 0x08 /* Private */,
      14,    0,  103,    2, 0x08 /* Private */,
      15,    0,  104,    2, 0x08 /* Private */,
      16,    0,  105,    2, 0x08 /* Private */,
      17,    0,  106,    2, 0x08 /* Private */,
      18,    0,  107,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Double, QMetaType::Double,    8,    9,
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

void f2c_cpp::ExplorationScreen::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ExplorationScreen *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->backRequested(); break;
        case 1: _t->startScanRequested(); break;
        case 2: _t->finishSaveMapRequested(); break;
        case 3: _t->startPlanningRequested(); break;
        case 4: _t->stopPipelineRequested(); break;
        case 5: _t->teleopTwistRequested((*reinterpret_cast< double(*)>(_a[1])),(*reinterpret_cast< double(*)>(_a[2]))); break;
        case 6: _t->teleopArmRequested(); break;
        case 7: _t->teleopDisarmRequested(); break;
        case 8: _t->teleopGprPowerOffRequested(); break;
        case 9: _t->onDashboardClicked(); break;
        case 10: _t->onStartScanClicked(); break;
        case 11: _t->onStartPlanningClicked(); break;
        case 12: _t->onStopPipelineClicked(); break;
        case 13: _t->onMappingLockTick(); break;
        case 14: _t->onTeleopPublishTick(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (ExplorationScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExplorationScreen::backRequested)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (ExplorationScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExplorationScreen::startScanRequested)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (ExplorationScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExplorationScreen::finishSaveMapRequested)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (ExplorationScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExplorationScreen::startPlanningRequested)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (ExplorationScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExplorationScreen::stopPipelineRequested)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (ExplorationScreen::*)(double , double );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExplorationScreen::teleopTwistRequested)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (ExplorationScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExplorationScreen::teleopArmRequested)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (ExplorationScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExplorationScreen::teleopDisarmRequested)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (ExplorationScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ExplorationScreen::teleopGprPowerOffRequested)) {
                *result = 8;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::ExplorationScreen::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__ExplorationScreen.data,
    qt_meta_data_f2c_cpp__ExplorationScreen,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::ExplorationScreen::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::ExplorationScreen::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__ExplorationScreen.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int f2c_cpp::ExplorationScreen::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void f2c_cpp::ExplorationScreen::backRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void f2c_cpp::ExplorationScreen::startScanRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void f2c_cpp::ExplorationScreen::finishSaveMapRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void f2c_cpp::ExplorationScreen::startPlanningRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void f2c_cpp::ExplorationScreen::stopPipelineRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void f2c_cpp::ExplorationScreen::teleopTwistRequested(double _t1, double _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void f2c_cpp::ExplorationScreen::teleopArmRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void f2c_cpp::ExplorationScreen::teleopDisarmRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void f2c_cpp::ExplorationScreen::teleopGprPowerOffRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
