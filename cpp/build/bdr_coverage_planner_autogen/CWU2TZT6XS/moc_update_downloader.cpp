/****************************************************************************
** Meta object code from reading C++ file 'update_downloader.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/update/update_downloader.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'update_downloader.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__update__UpdateDownloader_t {
    QByteArrayData data[20];
    char stringdata0[267];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__update__UpdateDownloader_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__update__UpdateDownloader_t qt_meta_stringdata_f2c_cpp__update__UpdateDownloader = {
    {
QT_MOC_LITERAL(0, 0, 33), // "f2c_cpp::update::UpdateDownlo..."
QT_MOC_LITERAL(1, 34, 15), // "progressChanged"
QT_MOC_LITERAL(2, 50, 0), // ""
QT_MOC_LITERAL(3, 51, 8), // "received"
QT_MOC_LITERAL(4, 60, 5), // "total"
QT_MOC_LITERAL(5, 66, 16), // "downloadComplete"
QT_MOC_LITERAL(6, 83, 7), // "debPath"
QT_MOC_LITERAL(7, 91, 14), // "downloadFailed"
QT_MOC_LITERAL(8, 106, 6), // "reason"
QT_MOC_LITERAL(9, 113, 9), // "cancelled"
QT_MOC_LITERAL(10, 123, 14), // "retryScheduled"
QT_MOC_LITERAL(11, 138, 7), // "attempt"
QT_MOC_LITERAL(12, 146, 13), // "totalAttempts"
QT_MOC_LITERAL(13, 160, 7), // "delayMs"
QT_MOC_LITERAL(14, 168, 14), // "onDebReadyRead"
QT_MOC_LITERAL(15, 183, 13), // "onDebFinished"
QT_MOC_LITERAL(16, 197, 16), // "onSha256Finished"
QT_MOC_LITERAL(17, 214, 14), // "onStallTimeout"
QT_MOC_LITERAL(18, 229, 17), // "onTotalCeilingHit"
QT_MOC_LITERAL(19, 247, 19) // "onRetryDelayElapsed"

    },
    "f2c_cpp::update::UpdateDownloader\0"
    "progressChanged\0\0received\0total\0"
    "downloadComplete\0debPath\0downloadFailed\0"
    "reason\0cancelled\0retryScheduled\0attempt\0"
    "totalAttempts\0delayMs\0onDebReadyRead\0"
    "onDebFinished\0onSha256Finished\0"
    "onStallTimeout\0onTotalCeilingHit\0"
    "onRetryDelayElapsed"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__update__UpdateDownloader[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      11,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       5,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   69,    2, 0x06 /* Public */,
       5,    1,   74,    2, 0x06 /* Public */,
       7,    1,   77,    2, 0x06 /* Public */,
       9,    0,   80,    2, 0x06 /* Public */,
      10,    3,   81,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      14,    0,   88,    2, 0x08 /* Private */,
      15,    0,   89,    2, 0x08 /* Private */,
      16,    0,   90,    2, 0x08 /* Private */,
      17,    0,   91,    2, 0x08 /* Private */,
      18,    0,   92,    2, 0x08 /* Private */,
      19,    0,   93,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::LongLong, QMetaType::LongLong,    3,    4,
    QMetaType::Void, QMetaType::QString,    6,
    QMetaType::Void, QMetaType::QString,    8,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::Int, QMetaType::Int,   11,   12,   13,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::update::UpdateDownloader::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<UpdateDownloader *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->progressChanged((*reinterpret_cast< qint64(*)>(_a[1])),(*reinterpret_cast< qint64(*)>(_a[2]))); break;
        case 1: _t->downloadComplete((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->downloadFailed((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->cancelled(); break;
        case 4: _t->retryScheduled((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2])),(*reinterpret_cast< int(*)>(_a[3]))); break;
        case 5: _t->onDebReadyRead(); break;
        case 6: _t->onDebFinished(); break;
        case 7: _t->onSha256Finished(); break;
        case 8: _t->onStallTimeout(); break;
        case 9: _t->onTotalCeilingHit(); break;
        case 10: _t->onRetryDelayElapsed(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (UpdateDownloader::*)(qint64 , qint64 );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateDownloader::progressChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (UpdateDownloader::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateDownloader::downloadComplete)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (UpdateDownloader::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateDownloader::downloadFailed)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (UpdateDownloader::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateDownloader::cancelled)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (UpdateDownloader::*)(int , int , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UpdateDownloader::retryScheduled)) {
                *result = 4;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::update::UpdateDownloader::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__update__UpdateDownloader.data,
    qt_meta_data_f2c_cpp__update__UpdateDownloader,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::update::UpdateDownloader::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::update::UpdateDownloader::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__update__UpdateDownloader.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int f2c_cpp::update::UpdateDownloader::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
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
void f2c_cpp::update::UpdateDownloader::progressChanged(qint64 _t1, qint64 _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void f2c_cpp::update::UpdateDownloader::downloadComplete(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void f2c_cpp::update::UpdateDownloader::downloadFailed(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void f2c_cpp::update::UpdateDownloader::cancelled()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void f2c_cpp::update::UpdateDownloader::retryScheduled(int _t1, int _t2, int _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
