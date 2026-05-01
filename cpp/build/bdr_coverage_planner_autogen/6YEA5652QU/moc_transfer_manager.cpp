/****************************************************************************
** Meta object code from reading C++ file 'transfer_manager.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/transfer_manager.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#include <QtCore/QList>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'transfer_manager.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__TransferManager_t {
    QByteArrayData data[40];
    char stringdata0[500];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__TransferManager_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__TransferManager_t qt_meta_stringdata_f2c_cpp__TransferManager = {
    {
QT_MOC_LITERAL(0, 0, 24), // "f2c_cpp::TransferManager"
QT_MOC_LITERAL(1, 25, 15), // "progressUpdated"
QT_MOC_LITERAL(2, 41, 0), // ""
QT_MOC_LITERAL(3, 42, 5), // "jobId"
QT_MOC_LITERAL(4, 48, 11), // "transferred"
QT_MOC_LITERAL(5, 60, 5), // "total"
QT_MOC_LITERAL(6, 66, 9), // "speedMBps"
QT_MOC_LITERAL(7, 76, 7), // "percent"
QT_MOC_LITERAL(8, 84, 11), // "currentFile"
QT_MOC_LITERAL(9, 96, 15), // "jobStateChanged"
QT_MOC_LITERAL(10, 112, 13), // "TransferState"
QT_MOC_LITERAL(11, 126, 8), // "newState"
QT_MOC_LITERAL(12, 135, 12), // "jobCompleted"
QT_MOC_LITERAL(13, 148, 7), // "success"
QT_MOC_LITERAL(14, 156, 7), // "message"
QT_MOC_LITERAL(15, 164, 12), // "queueChanged"
QT_MOC_LITERAL(16, 177, 16), // "connectionStatus"
QT_MOC_LITERAL(17, 194, 9), // "connected"
QT_MOC_LITERAL(18, 204, 14), // "datesAvailable"
QT_MOC_LITERAL(19, 219, 5), // "dates"
QT_MOC_LITERAL(20, 225, 17), // "sectionsAvailable"
QT_MOC_LITERAL(21, 243, 4), // "date"
QT_MOC_LITERAL(22, 248, 18), // "QList<SectionInfo>"
QT_MOC_LITERAL(23, 267, 8), // "sections"
QT_MOC_LITERAL(24, 276, 19), // "sectionDetailsReady"
QT_MOC_LITERAL(25, 296, 11), // "SectionInfo"
QT_MOC_LITERAL(26, 308, 7), // "section"
QT_MOC_LITERAL(27, 316, 10), // "queryError"
QT_MOC_LITERAL(28, 327, 9), // "operation"
QT_MOC_LITERAL(29, 337, 5), // "error"
QT_MOC_LITERAL(30, 343, 18), // "onProcessReadyRead"
QT_MOC_LITERAL(31, 362, 17), // "onProcessFinished"
QT_MOC_LITERAL(32, 380, 8), // "exitCode"
QT_MOC_LITERAL(33, 389, 20), // "QProcess::ExitStatus"
QT_MOC_LITERAL(34, 410, 6), // "status"
QT_MOC_LITERAL(35, 417, 14), // "onProcessError"
QT_MOC_LITERAL(36, 432, 22), // "QProcess::ProcessError"
QT_MOC_LITERAL(37, 455, 12), // "onRetryTimer"
QT_MOC_LITERAL(38, 468, 12), // "startNextJob"
QT_MOC_LITERAL(39, 481, 18) // "checkProcessStatus"

    },
    "f2c_cpp::TransferManager\0progressUpdated\0"
    "\0jobId\0transferred\0total\0speedMBps\0"
    "percent\0currentFile\0jobStateChanged\0"
    "TransferState\0newState\0jobCompleted\0"
    "success\0message\0queueChanged\0"
    "connectionStatus\0connected\0datesAvailable\0"
    "dates\0sectionsAvailable\0date\0"
    "QList<SectionInfo>\0sections\0"
    "sectionDetailsReady\0SectionInfo\0section\0"
    "queryError\0operation\0error\0"
    "onProcessReadyRead\0onProcessFinished\0"
    "exitCode\0QProcess::ExitStatus\0status\0"
    "onProcessError\0QProcess::ProcessError\0"
    "onRetryTimer\0startNextJob\0checkProcessStatus"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__TransferManager[] = {

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
       1,    6,   89,    2, 0x06 /* Public */,
       9,    2,  102,    2, 0x06 /* Public */,
      12,    3,  107,    2, 0x06 /* Public */,
      15,    0,  114,    2, 0x06 /* Public */,
      16,    2,  115,    2, 0x06 /* Public */,
      18,    1,  120,    2, 0x06 /* Public */,
      20,    2,  123,    2, 0x06 /* Public */,
      24,    1,  128,    2, 0x06 /* Public */,
      27,    2,  131,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      30,    0,  136,    2, 0x08 /* Private */,
      31,    2,  137,    2, 0x08 /* Private */,
      35,    1,  142,    2, 0x08 /* Private */,
      37,    0,  145,    2, 0x08 /* Private */,
      38,    0,  146,    2, 0x08 /* Private */,
      39,    0,  147,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::Int, QMetaType::LongLong, QMetaType::LongLong, QMetaType::Double, QMetaType::Int, QMetaType::QString,    3,    4,    5,    6,    7,    8,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 10,    3,   11,
    QMetaType::Void, QMetaType::Int, QMetaType::Bool, QMetaType::QString,    3,   13,   14,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,   17,   14,
    QMetaType::Void, QMetaType::QStringList,   19,
    QMetaType::Void, QMetaType::QString, 0x80000000 | 22,   21,   23,
    QMetaType::Void, 0x80000000 | 25,   26,
    QMetaType::Void, QMetaType::QString, QMetaType::QString,   28,   29,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 33,   32,   34,
    QMetaType::Void, 0x80000000 | 36,   29,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::TransferManager::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<TransferManager *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->progressUpdated((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< qint64(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3])),(*reinterpret_cast< double(*)>(_a[4])),(*reinterpret_cast< int(*)>(_a[5])),(*reinterpret_cast< const QString(*)>(_a[6]))); break;
        case 1: _t->jobStateChanged((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< TransferState(*)>(_a[2]))); break;
        case 2: _t->jobCompleted((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 3: _t->queueChanged(); break;
        case 4: _t->connectionStatus((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 5: _t->datesAvailable((*reinterpret_cast< const QStringList(*)>(_a[1]))); break;
        case 6: _t->sectionsAvailable((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QList<SectionInfo>(*)>(_a[2]))); break;
        case 7: _t->sectionDetailsReady((*reinterpret_cast< const SectionInfo(*)>(_a[1]))); break;
        case 8: _t->queryError((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 9: _t->onProcessReadyRead(); break;
        case 10: _t->onProcessFinished((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< QProcess::ExitStatus(*)>(_a[2]))); break;
        case 11: _t->onProcessError((*reinterpret_cast< QProcess::ProcessError(*)>(_a[1]))); break;
        case 12: _t->onRetryTimer(); break;
        case 13: _t->startNextJob(); break;
        case 14: _t->checkProcessStatus(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (TransferManager::*)(int , qint64 , qint64 , double , int , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TransferManager::progressUpdated)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (TransferManager::*)(int , TransferState );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TransferManager::jobStateChanged)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (TransferManager::*)(int , bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TransferManager::jobCompleted)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (TransferManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TransferManager::queueChanged)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (TransferManager::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TransferManager::connectionStatus)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (TransferManager::*)(const QStringList & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TransferManager::datesAvailable)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (TransferManager::*)(const QString & , const QList<SectionInfo> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TransferManager::sectionsAvailable)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (TransferManager::*)(const SectionInfo & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TransferManager::sectionDetailsReady)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (TransferManager::*)(const QString & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&TransferManager::queryError)) {
                *result = 8;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::TransferManager::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__TransferManager.data,
    qt_meta_data_f2c_cpp__TransferManager,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::TransferManager::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::TransferManager::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__TransferManager.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int f2c_cpp::TransferManager::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
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
void f2c_cpp::TransferManager::progressUpdated(int _t1, qint64 _t2, qint64 _t3, double _t4, int _t5, const QString & _t6)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t6))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void f2c_cpp::TransferManager::jobStateChanged(int _t1, TransferState _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void f2c_cpp::TransferManager::jobCompleted(int _t1, bool _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void f2c_cpp::TransferManager::queueChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void f2c_cpp::TransferManager::connectionStatus(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void f2c_cpp::TransferManager::datesAvailable(const QStringList & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void f2c_cpp::TransferManager::sectionsAvailable(const QString & _t1, const QList<SectionInfo> & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void f2c_cpp::TransferManager::sectionDetailsReady(const SectionInfo & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void f2c_cpp::TransferManager::queryError(const QString & _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
