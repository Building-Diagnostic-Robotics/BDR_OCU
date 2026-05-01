/****************************************************************************
** Meta object code from reading C++ file 'fpv_camera_view.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/components/fpv_camera_view.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'fpv_camera_view.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__FPVCameraView_t {
    QByteArrayData data[7];
    char stringdata0[84];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__FPVCameraView_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__FPVCameraView_t qt_meta_stringdata_f2c_cpp__FPVCameraView = {
    {
QT_MOC_LITERAL(0, 0, 22), // "f2c_cpp::FPVCameraView"
QT_MOC_LITERAL(1, 23, 13), // "streamStarted"
QT_MOC_LITERAL(2, 37, 0), // ""
QT_MOC_LITERAL(3, 38, 13), // "streamStopped"
QT_MOC_LITERAL(4, 52, 11), // "streamError"
QT_MOC_LITERAL(5, 64, 3), // "msg"
QT_MOC_LITERAL(6, 68, 15) // "firstFrameReady"

    },
    "f2c_cpp::FPVCameraView\0streamStarted\0"
    "\0streamStopped\0streamError\0msg\0"
    "firstFrameReady"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__FPVCameraView[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       4,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   34,    2, 0x06 /* Public */,
       3,    0,   35,    2, 0x06 /* Public */,
       4,    1,   36,    2, 0x06 /* Public */,
       6,    0,   39,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    5,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::FPVCameraView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<FPVCameraView *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->streamStarted(); break;
        case 1: _t->streamStopped(); break;
        case 2: _t->streamError((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->firstFrameReady(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (FPVCameraView::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FPVCameraView::streamStarted)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (FPVCameraView::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FPVCameraView::streamStopped)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (FPVCameraView::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FPVCameraView::streamError)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (FPVCameraView::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FPVCameraView::firstFrameReady)) {
                *result = 3;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::FPVCameraView::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__FPVCameraView.data,
    qt_meta_data_f2c_cpp__FPVCameraView,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::FPVCameraView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::FPVCameraView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__FPVCameraView.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int f2c_cpp::FPVCameraView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 4)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 4;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::FPVCameraView::streamStarted()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void f2c_cpp::FPVCameraView::streamStopped()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void f2c_cpp::FPVCameraView::streamError(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void f2c_cpp::FPVCameraView::firstFrameReady()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
