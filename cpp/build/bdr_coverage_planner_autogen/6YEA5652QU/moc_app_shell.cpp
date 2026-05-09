/****************************************************************************
** Meta object code from reading C++ file 'app_shell.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/app_shell.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'app_shell.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__AppShellWindow_t {
    QByteArrayData data[41];
    char stringdata0[915];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__AppShellWindow_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__AppShellWindow_t qt_meta_stringdata_f2c_cpp__AppShellWindow = {
    {
QT_MOC_LITERAL(0, 0, 23), // "f2c_cpp::AppShellWindow"
QT_MOC_LITERAL(1, 24, 11), // "bootHealthy"
QT_MOC_LITERAL(2, 36, 0), // ""
QT_MOC_LITERAL(3, 37, 16), // "onLoginSubmitted"
QT_MOC_LITERAL(4, 54, 7), // "robotId"
QT_MOC_LITERAL(5, 62, 10), // "accessCode"
QT_MOC_LITERAL(6, 73, 10), // "goToStage1"
QT_MOC_LITERAL(7, 84, 10), // "goToStage2"
QT_MOC_LITERAL(8, 95, 10), // "goToStage3"
QT_MOC_LITERAL(9, 106, 10), // "goToStage4"
QT_MOC_LITERAL(10, 117, 10), // "goToStage5"
QT_MOC_LITERAL(11, 128, 20), // "onThemeToggleChanged"
QT_MOC_LITERAL(12, 149, 9), // "dark_mode"
QT_MOC_LITERAL(13, 159, 14), // "onStartNewScan"
QT_MOC_LITERAL(14, 174, 31), // "onExplorationStartScanRequested"
QT_MOC_LITERAL(15, 206, 35), // "onExplorationFinishSaveMapReq..."
QT_MOC_LITERAL(16, 242, 35), // "onExplorationStartPlanningReq..."
QT_MOC_LITERAL(17, 278, 34), // "onExplorationStopPipelineRequ..."
QT_MOC_LITERAL(18, 313, 33), // "onExplorationTeleopTwistReque..."
QT_MOC_LITERAL(19, 347, 8), // "linear_x"
QT_MOC_LITERAL(20, 356, 9), // "angular_z"
QT_MOC_LITERAL(21, 366, 31), // "onExplorationTeleopArmRequested"
QT_MOC_LITERAL(22, 398, 34), // "onExplorationTeleopDisarmRequ..."
QT_MOC_LITERAL(23, 433, 39), // "onExplorationTeleopGprPowerOf..."
QT_MOC_LITERAL(24, 473, 37), // "onPlannerPublishScanSegmentsR..."
QT_MOC_LITERAL(25, 511, 19), // "std::vector<double>"
QT_MOC_LITERAL(26, 531, 8), // "xy_pairs"
QT_MOC_LITERAL(27, 540, 35), // "onPlannerStartScanSegmentsReq..."
QT_MOC_LITERAL(28, 576, 16), // "progression_mode"
QT_MOC_LITERAL(29, 593, 27), // "onPlannerScanStartRequested"
QT_MOC_LITERAL(30, 621, 9), // "speed_mps"
QT_MOC_LITERAL(31, 631, 27), // "onPlannerScanPauseRequested"
QT_MOC_LITERAL(32, 659, 28), // "onPlannerScanResumeRequested"
QT_MOC_LITERAL(33, 688, 25), // "onPlannerWakeGprRequested"
QT_MOC_LITERAL(34, 714, 31), // "onPlannerEmergencyStopRequested"
QT_MOC_LITERAL(35, 746, 33), // "onPlannerCompleteMissionReque..."
QT_MOC_LITERAL(36, 780, 28), // "onPlannerCancelScanRequested"
QT_MOC_LITERAL(37, 809, 29), // "onPlannerDiscardScanRequested"
QT_MOC_LITERAL(38, 839, 23), // "onExplorationLaunchPoll"
QT_MOC_LITERAL(39, 863, 25), // "onExplorationLiveFastTick"
QT_MOC_LITERAL(40, 889, 25) // "onExplorationLiveSlowTick"

    },
    "f2c_cpp::AppShellWindow\0bootHealthy\0"
    "\0onLoginSubmitted\0robotId\0accessCode\0"
    "goToStage1\0goToStage2\0goToStage3\0"
    "goToStage4\0goToStage5\0onThemeToggleChanged\0"
    "dark_mode\0onStartNewScan\0"
    "onExplorationStartScanRequested\0"
    "onExplorationFinishSaveMapRequested\0"
    "onExplorationStartPlanningRequested\0"
    "onExplorationStopPipelineRequested\0"
    "onExplorationTeleopTwistRequested\0"
    "linear_x\0angular_z\0onExplorationTeleopArmRequested\0"
    "onExplorationTeleopDisarmRequested\0"
    "onExplorationTeleopGprPowerOffRequested\0"
    "onPlannerPublishScanSegmentsRequested\0"
    "std::vector<double>\0xy_pairs\0"
    "onPlannerStartScanSegmentsRequested\0"
    "progression_mode\0onPlannerScanStartRequested\0"
    "speed_mps\0onPlannerScanPauseRequested\0"
    "onPlannerScanResumeRequested\0"
    "onPlannerWakeGprRequested\0"
    "onPlannerEmergencyStopRequested\0"
    "onPlannerCompleteMissionRequested\0"
    "onPlannerCancelScanRequested\0"
    "onPlannerDiscardScanRequested\0"
    "onExplorationLaunchPoll\0"
    "onExplorationLiveFastTick\0"
    "onExplorationLiveSlowTick"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__AppShellWindow[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      30,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       1,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,  164,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       3,    2,  165,    2, 0x08 /* Private */,
       6,    0,  170,    2, 0x08 /* Private */,
       7,    0,  171,    2, 0x08 /* Private */,
       8,    0,  172,    2, 0x08 /* Private */,
       9,    0,  173,    2, 0x08 /* Private */,
      10,    0,  174,    2, 0x08 /* Private */,
      11,    1,  175,    2, 0x08 /* Private */,
      13,    0,  178,    2, 0x08 /* Private */,
      14,    0,  179,    2, 0x08 /* Private */,
      15,    0,  180,    2, 0x08 /* Private */,
      16,    0,  181,    2, 0x08 /* Private */,
      17,    0,  182,    2, 0x08 /* Private */,
      18,    2,  183,    2, 0x08 /* Private */,
      21,    0,  188,    2, 0x08 /* Private */,
      22,    0,  189,    2, 0x08 /* Private */,
      23,    0,  190,    2, 0x08 /* Private */,
      24,    1,  191,    2, 0x08 /* Private */,
      27,    1,  194,    2, 0x08 /* Private */,
      29,    1,  197,    2, 0x08 /* Private */,
      31,    0,  200,    2, 0x08 /* Private */,
      32,    0,  201,    2, 0x08 /* Private */,
      33,    0,  202,    2, 0x08 /* Private */,
      34,    0,  203,    2, 0x08 /* Private */,
      35,    0,  204,    2, 0x08 /* Private */,
      36,    0,  205,    2, 0x08 /* Private */,
      37,    0,  206,    2, 0x08 /* Private */,
      38,    0,  207,    2, 0x08 /* Private */,
      39,    0,  208,    2, 0x08 /* Private */,
      40,    0,  209,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,

 // slots: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::QString,    4,    5,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,   12,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Double, QMetaType::Double,   19,   20,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 25,   26,
    QMetaType::Void, QMetaType::QString,   28,
    QMetaType::Void, QMetaType::Double,   30,
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

       0        // eod
};

void f2c_cpp::AppShellWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<AppShellWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->bootHealthy(); break;
        case 1: _t->onLoginSubmitted((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 2: _t->goToStage1(); break;
        case 3: _t->goToStage2(); break;
        case 4: _t->goToStage3(); break;
        case 5: _t->goToStage4(); break;
        case 6: _t->goToStage5(); break;
        case 7: _t->onThemeToggleChanged((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 8: _t->onStartNewScan(); break;
        case 9: _t->onExplorationStartScanRequested(); break;
        case 10: _t->onExplorationFinishSaveMapRequested(); break;
        case 11: _t->onExplorationStartPlanningRequested(); break;
        case 12: _t->onExplorationStopPipelineRequested(); break;
        case 13: _t->onExplorationTeleopTwistRequested((*reinterpret_cast< double(*)>(_a[1])),(*reinterpret_cast< double(*)>(_a[2]))); break;
        case 14: _t->onExplorationTeleopArmRequested(); break;
        case 15: _t->onExplorationTeleopDisarmRequested(); break;
        case 16: _t->onExplorationTeleopGprPowerOffRequested(); break;
        case 17: _t->onPlannerPublishScanSegmentsRequested((*reinterpret_cast< const std::vector<double>(*)>(_a[1]))); break;
        case 18: _t->onPlannerStartScanSegmentsRequested((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 19: _t->onPlannerScanStartRequested((*reinterpret_cast< double(*)>(_a[1]))); break;
        case 20: _t->onPlannerScanPauseRequested(); break;
        case 21: _t->onPlannerScanResumeRequested(); break;
        case 22: _t->onPlannerWakeGprRequested(); break;
        case 23: _t->onPlannerEmergencyStopRequested(); break;
        case 24: _t->onPlannerCompleteMissionRequested(); break;
        case 25: _t->onPlannerCancelScanRequested(); break;
        case 26: _t->onPlannerDiscardScanRequested(); break;
        case 27: _t->onExplorationLaunchPoll(); break;
        case 28: _t->onExplorationLiveFastTick(); break;
        case 29: _t->onExplorationLiveSlowTick(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (AppShellWindow::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AppShellWindow::bootHealthy)) {
                *result = 0;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::AppShellWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__AppShellWindow.data,
    qt_meta_data_f2c_cpp__AppShellWindow,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::AppShellWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::AppShellWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__AppShellWindow.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int f2c_cpp::AppShellWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 30)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 30;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 30)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 30;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::AppShellWindow::bootHealthy()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
