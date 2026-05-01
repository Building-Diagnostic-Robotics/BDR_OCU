/****************************************************************************
** Meta object code from reading C++ file 'planner_screen.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/planner_screen.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'planner_screen.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__PlannerScreen_t {
    QByteArrayData data[22];
    char stringdata0[417];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__PlannerScreen_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__PlannerScreen_t qt_meta_stringdata_f2c_cpp__PlannerScreen = {
    {
QT_MOC_LITERAL(0, 0, 22), // "f2c_cpp::PlannerScreen"
QT_MOC_LITERAL(1, 23, 13), // "backRequested"
QT_MOC_LITERAL(2, 37, 0), // ""
QT_MOC_LITERAL(3, 38, 18), // "nextStageRequested"
QT_MOC_LITERAL(4, 57, 28), // "publishScanSegmentsRequested"
QT_MOC_LITERAL(5, 86, 19), // "std::vector<double>"
QT_MOC_LITERAL(6, 106, 8), // "xy_pairs"
QT_MOC_LITERAL(7, 115, 26), // "startScanSegmentsRequested"
QT_MOC_LITERAL(8, 142, 16), // "progression_mode"
QT_MOC_LITERAL(9, 159, 18), // "scanStartRequested"
QT_MOC_LITERAL(10, 178, 18), // "scanPauseRequested"
QT_MOC_LITERAL(11, 197, 19), // "scanResumeRequested"
QT_MOC_LITERAL(12, 217, 22), // "emergencyStopRequested"
QT_MOC_LITERAL(13, 240, 24), // "scanTeleopTwistRequested"
QT_MOC_LITERAL(14, 265, 8), // "linear_x"
QT_MOC_LITERAL(15, 274, 9), // "angular_z"
QT_MOC_LITERAL(16, 284, 22), // "scanTeleopArmRequested"
QT_MOC_LITERAL(17, 307, 25), // "scanTeleopDisarmRequested"
QT_MOC_LITERAL(18, 333, 30), // "scanTeleopGprPowerOffRequested"
QT_MOC_LITERAL(19, 364, 24), // "completeMissionRequested"
QT_MOC_LITERAL(20, 389, 13), // "onBackClicked"
QT_MOC_LITERAL(21, 403, 13) // "onNextClicked"

    },
    "f2c_cpp::PlannerScreen\0backRequested\0"
    "\0nextStageRequested\0publishScanSegmentsRequested\0"
    "std::vector<double>\0xy_pairs\0"
    "startScanSegmentsRequested\0progression_mode\0"
    "scanStartRequested\0scanPauseRequested\0"
    "scanResumeRequested\0emergencyStopRequested\0"
    "scanTeleopTwistRequested\0linear_x\0"
    "angular_z\0scanTeleopArmRequested\0"
    "scanTeleopDisarmRequested\0"
    "scanTeleopGprPowerOffRequested\0"
    "completeMissionRequested\0onBackClicked\0"
    "onNextClicked"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__PlannerScreen[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      15,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      13,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   89,    2, 0x06 /* Public */,
       3,    0,   90,    2, 0x06 /* Public */,
       4,    1,   91,    2, 0x06 /* Public */,
       7,    1,   94,    2, 0x06 /* Public */,
       9,    0,   97,    2, 0x06 /* Public */,
      10,    0,   98,    2, 0x06 /* Public */,
      11,    0,   99,    2, 0x06 /* Public */,
      12,    0,  100,    2, 0x06 /* Public */,
      13,    2,  101,    2, 0x06 /* Public */,
      16,    0,  106,    2, 0x06 /* Public */,
      17,    0,  107,    2, 0x06 /* Public */,
      18,    0,  108,    2, 0x06 /* Public */,
      19,    0,  109,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      20,    0,  110,    2, 0x08 /* Private */,
      21,    0,  111,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 5,    6,
    QMetaType::Void, QMetaType::QString,    8,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Double, QMetaType::Double,   14,   15,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::PlannerScreen::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<PlannerScreen *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->backRequested(); break;
        case 1: _t->nextStageRequested(); break;
        case 2: _t->publishScanSegmentsRequested((*reinterpret_cast< const std::vector<double>(*)>(_a[1]))); break;
        case 3: _t->startScanSegmentsRequested((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: _t->scanStartRequested(); break;
        case 5: _t->scanPauseRequested(); break;
        case 6: _t->scanResumeRequested(); break;
        case 7: _t->emergencyStopRequested(); break;
        case 8: _t->scanTeleopTwistRequested((*reinterpret_cast< double(*)>(_a[1])),(*reinterpret_cast< double(*)>(_a[2]))); break;
        case 9: _t->scanTeleopArmRequested(); break;
        case 10: _t->scanTeleopDisarmRequested(); break;
        case 11: _t->scanTeleopGprPowerOffRequested(); break;
        case 12: _t->completeMissionRequested(); break;
        case 13: _t->onBackClicked(); break;
        case 14: _t->onNextClicked(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::backRequested)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::nextStageRequested)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)(const std::vector<double> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::publishScanSegmentsRequested)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::startScanSegmentsRequested)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::scanStartRequested)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::scanPauseRequested)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::scanResumeRequested)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::emergencyStopRequested)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)(double , double );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::scanTeleopTwistRequested)) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::scanTeleopArmRequested)) {
                *result = 9;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::scanTeleopDisarmRequested)) {
                *result = 10;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::scanTeleopGprPowerOffRequested)) {
                *result = 11;
                return;
            }
        }
        {
            using _t = void (PlannerScreen::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlannerScreen::completeMissionRequested)) {
                *result = 12;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::PlannerScreen::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__PlannerScreen.data,
    qt_meta_data_f2c_cpp__PlannerScreen,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::PlannerScreen::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::PlannerScreen::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__PlannerScreen.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int f2c_cpp::PlannerScreen::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void f2c_cpp::PlannerScreen::backRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void f2c_cpp::PlannerScreen::nextStageRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void f2c_cpp::PlannerScreen::publishScanSegmentsRequested(const std::vector<double> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void f2c_cpp::PlannerScreen::startScanSegmentsRequested(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void f2c_cpp::PlannerScreen::scanStartRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void f2c_cpp::PlannerScreen::scanPauseRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void f2c_cpp::PlannerScreen::scanResumeRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void f2c_cpp::PlannerScreen::emergencyStopRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void f2c_cpp::PlannerScreen::scanTeleopTwistRequested(double _t1, double _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}

// SIGNAL 9
void f2c_cpp::PlannerScreen::scanTeleopArmRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 9, nullptr);
}

// SIGNAL 10
void f2c_cpp::PlannerScreen::scanTeleopDisarmRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void f2c_cpp::PlannerScreen::scanTeleopGprPowerOffRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 11, nullptr);
}

// SIGNAL 12
void f2c_cpp::PlannerScreen::completeMissionRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 12, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
