/****************************************************************************
** Meta object code from reading C++ file 'cloud_upload_manager.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../cpp/include/cloud_upload_manager.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'cloud_upload_manager.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__CloudUploadManager_t {
    QByteArrayData data[31];
    char stringdata0[395];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__CloudUploadManager_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__CloudUploadManager_t qt_meta_stringdata_f2c_cpp__CloudUploadManager = {
    {
QT_MOC_LITERAL(0, 0, 27), // "f2c_cpp::CloudUploadManager"
QT_MOC_LITERAL(1, 28, 14), // "uploadProgress"
QT_MOC_LITERAL(2, 43, 0), // ""
QT_MOC_LITERAL(3, 44, 5), // "jobId"
QT_MOC_LITERAL(4, 50, 8), // "uploaded"
QT_MOC_LITERAL(5, 59, 5), // "total"
QT_MOC_LITERAL(6, 65, 9), // "speedMBps"
QT_MOC_LITERAL(7, 75, 7), // "percent"
QT_MOC_LITERAL(8, 83, 18), // "uploadStateChanged"
QT_MOC_LITERAL(9, 102, 11), // "UploadState"
QT_MOC_LITERAL(10, 114, 8), // "newState"
QT_MOC_LITERAL(11, 123, 15), // "uploadCompleted"
QT_MOC_LITERAL(12, 139, 7), // "success"
QT_MOC_LITERAL(13, 147, 7), // "message"
QT_MOC_LITERAL(14, 155, 12), // "queueChanged"
QT_MOC_LITERAL(15, 168, 13), // "geocodeResult"
QT_MOC_LITERAL(16, 182, 7), // "address"
QT_MOC_LITERAL(17, 190, 14), // "uploadVerified"
QT_MOC_LITERAL(18, 205, 11), // "sectionPath"
QT_MOC_LITERAL(19, 217, 13), // "existsInCloud"
QT_MOC_LITERAL(20, 231, 17), // "onProcessFinished"
QT_MOC_LITERAL(21, 249, 8), // "exitCode"
QT_MOC_LITERAL(22, 258, 20), // "QProcess::ExitStatus"
QT_MOC_LITERAL(23, 279, 6), // "status"
QT_MOC_LITERAL(24, 286, 18), // "onProcessReadyRead"
QT_MOC_LITERAL(25, 305, 14), // "onProcessError"
QT_MOC_LITERAL(26, 320, 22), // "QProcess::ProcessError"
QT_MOC_LITERAL(27, 343, 5), // "error"
QT_MOC_LITERAL(28, 349, 12), // "onRetryTimer"
QT_MOC_LITERAL(29, 362, 12), // "startNextJob"
QT_MOC_LITERAL(30, 375, 19) // "onProgressPollTimer"

    },
    "f2c_cpp::CloudUploadManager\0uploadProgress\0"
    "\0jobId\0uploaded\0total\0speedMBps\0percent\0"
    "uploadStateChanged\0UploadState\0newState\0"
    "uploadCompleted\0success\0message\0"
    "queueChanged\0geocodeResult\0address\0"
    "uploadVerified\0sectionPath\0existsInCloud\0"
    "onProcessFinished\0exitCode\0"
    "QProcess::ExitStatus\0status\0"
    "onProcessReadyRead\0onProcessError\0"
    "QProcess::ProcessError\0error\0onRetryTimer\0"
    "startNextJob\0onProgressPollTimer"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__CloudUploadManager[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      12,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       6,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    5,   74,    2, 0x06 /* Public */,
       8,    2,   85,    2, 0x06 /* Public */,
      11,    3,   90,    2, 0x06 /* Public */,
      14,    0,   97,    2, 0x06 /* Public */,
      15,    1,   98,    2, 0x06 /* Public */,
      17,    2,  101,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      20,    2,  106,    2, 0x08 /* Private */,
      24,    0,  111,    2, 0x08 /* Private */,
      25,    1,  112,    2, 0x08 /* Private */,
      28,    0,  115,    2, 0x08 /* Private */,
      29,    0,  116,    2, 0x08 /* Private */,
      30,    0,  117,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::Int, QMetaType::LongLong, QMetaType::LongLong, QMetaType::Double, QMetaType::Int,    3,    4,    5,    6,    7,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 9,    3,   10,
    QMetaType::Void, QMetaType::Int, QMetaType::Bool, QMetaType::QString,    3,   12,   13,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   16,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool,   18,   19,

 // slots: parameters
    QMetaType::Void, QMetaType::Int, 0x80000000 | 22,   21,   23,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 26,   27,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::CloudUploadManager::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<CloudUploadManager *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->uploadProgress((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< qint64(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3])),(*reinterpret_cast< double(*)>(_a[4])),(*reinterpret_cast< int(*)>(_a[5]))); break;
        case 1: _t->uploadStateChanged((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< UploadState(*)>(_a[2]))); break;
        case 2: _t->uploadCompleted((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 3: _t->queueChanged(); break;
        case 4: _t->geocodeResult((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 5: _t->uploadVerified((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2]))); break;
        case 6: _t->onProcessFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< QProcess::ExitStatus(*)>(_a[2]))); break;
        case 7: _t->onProcessReadyRead(); break;
        case 8: _t->onProcessError((*reinterpret_cast< QProcess::ProcessError(*)>(_a[1]))); break;
        case 9: _t->onRetryTimer(); break;
        case 10: _t->startNextJob(); break;
        case 11: _t->onProgressPollTimer(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (CloudUploadManager::*)(int , qint64 , qint64 , double , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CloudUploadManager::uploadProgress)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (CloudUploadManager::*)(int , UploadState );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CloudUploadManager::uploadStateChanged)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (CloudUploadManager::*)(int , bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CloudUploadManager::uploadCompleted)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (CloudUploadManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CloudUploadManager::queueChanged)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (CloudUploadManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CloudUploadManager::geocodeResult)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (CloudUploadManager::*)(const QString & , bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CloudUploadManager::uploadVerified)) {
                *result = 5;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::CloudUploadManager::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__CloudUploadManager.data,
    qt_meta_data_f2c_cpp__CloudUploadManager,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::CloudUploadManager::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::CloudUploadManager::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__CloudUploadManager.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int f2c_cpp::CloudUploadManager::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 12)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 12;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 12)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 12;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::CloudUploadManager::uploadProgress(int _t1, qint64 _t2, qint64 _t3, double _t4, int _t5)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void f2c_cpp::CloudUploadManager::uploadStateChanged(int _t1, UploadState _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void f2c_cpp::CloudUploadManager::uploadCompleted(int _t1, bool _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void f2c_cpp::CloudUploadManager::queueChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void f2c_cpp::CloudUploadManager::geocodeResult(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void f2c_cpp::CloudUploadManager::uploadVerified(const QString & _t1, bool _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
