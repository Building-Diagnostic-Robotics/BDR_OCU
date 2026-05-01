/****************************************************************************
** Meta object code from reading C++ file 'cloud_upload_dialog.hpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/cloud_upload_dialog.hpp"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'cloud_upload_dialog.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_f2c_cpp__AwsSettingsDialog_t {
    QByteArrayData data[6];
    char stringdata0[77];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__AwsSettingsDialog_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__AwsSettingsDialog_t qt_meta_stringdata_f2c_cpp__AwsSettingsDialog = {
    {
QT_MOC_LITERAL(0, 0, 26), // "f2c_cpp::AwsSettingsDialog"
QT_MOC_LITERAL(1, 27, 16), // "onTestConnection"
QT_MOC_LITERAL(2, 44, 0), // ""
QT_MOC_LITERAL(3, 45, 14), // "onTestComplete"
QT_MOC_LITERAL(4, 60, 9), // "AwsStatus"
QT_MOC_LITERAL(5, 70, 6) // "status"

    },
    "f2c_cpp::AwsSettingsDialog\0onTestConnection\0"
    "\0onTestComplete\0AwsStatus\0status"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__AwsSettingsDialog[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       2,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    0,   24,    2, 0x08 /* Private */,
       3,    1,   25,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 4,    5,

       0        // eod
};

void f2c_cpp::AwsSettingsDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<AwsSettingsDialog *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->onTestConnection(); break;
        case 1: _t->onTestComplete((*reinterpret_cast< const AwsStatus(*)>(_a[1]))); break;
        default: ;
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::AwsSettingsDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__AwsSettingsDialog.data,
    qt_meta_data_f2c_cpp__AwsSettingsDialog,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::AwsSettingsDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::AwsSettingsDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__AwsSettingsDialog.stringdata0))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int f2c_cpp::AwsSettingsDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDialog::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 2)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 2)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 2;
    }
    return _id;
}
struct qt_meta_stringdata_f2c_cpp__CloudUploadDialog_t {
    QByteArrayData data[41];
    char stringdata0[522];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_f2c_cpp__CloudUploadDialog_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_f2c_cpp__CloudUploadDialog_t qt_meta_stringdata_f2c_cpp__CloudUploadDialog = {
    {
QT_MOC_LITERAL(0, 0, 26), // "f2c_cpp::CloudUploadDialog"
QT_MOC_LITERAL(1, 27, 12), // "uploadActive"
QT_MOC_LITERAL(2, 40, 0), // ""
QT_MOC_LITERAL(3, 41, 6), // "active"
QT_MOC_LITERAL(4, 48, 15), // "refreshSections"
QT_MOC_LITERAL(5, 64, 11), // "onSelectAll"
QT_MOC_LITERAL(6, 76, 13), // "onDeselectAll"
QT_MOC_LITERAL(7, 90, 15), // "onUploadClicked"
QT_MOC_LITERAL(8, 106, 14), // "onPauseClicked"
QT_MOC_LITERAL(9, 121, 15), // "onCancelClicked"
QT_MOC_LITERAL(10, 137, 13), // "onItemChanged"
QT_MOC_LITERAL(11, 151, 16), // "QTreeWidgetItem*"
QT_MOC_LITERAL(12, 168, 4), // "item"
QT_MOC_LITERAL(13, 173, 6), // "column"
QT_MOC_LITERAL(14, 180, 20), // "onCurrentItemChanged"
QT_MOC_LITERAL(15, 201, 7), // "current"
QT_MOC_LITERAL(16, 209, 8), // "previous"
QT_MOC_LITERAL(17, 218, 22), // "onNetworkStatusUpdated"
QT_MOC_LITERAL(18, 241, 13), // "NetworkStatus"
QT_MOC_LITERAL(19, 255, 6), // "status"
QT_MOC_LITERAL(20, 262, 23), // "onAwsValidationComplete"
QT_MOC_LITERAL(21, 286, 9), // "AwsStatus"
QT_MOC_LITERAL(22, 296, 16), // "onUploadProgress"
QT_MOC_LITERAL(23, 313, 5), // "jobId"
QT_MOC_LITERAL(24, 319, 8), // "uploaded"
QT_MOC_LITERAL(25, 328, 5), // "total"
QT_MOC_LITERAL(26, 334, 9), // "speedMBps"
QT_MOC_LITERAL(27, 344, 7), // "percent"
QT_MOC_LITERAL(28, 352, 20), // "onUploadStateChanged"
QT_MOC_LITERAL(29, 373, 11), // "UploadState"
QT_MOC_LITERAL(30, 385, 5), // "state"
QT_MOC_LITERAL(31, 391, 17), // "onUploadCompleted"
QT_MOC_LITERAL(32, 409, 7), // "success"
QT_MOC_LITERAL(33, 417, 7), // "message"
QT_MOC_LITERAL(34, 425, 14), // "onQueueChanged"
QT_MOC_LITERAL(35, 440, 15), // "onGeocodeResult"
QT_MOC_LITERAL(36, 456, 7), // "address"
QT_MOC_LITERAL(37, 464, 16), // "onUploadVerified"
QT_MOC_LITERAL(38, 481, 11), // "sectionPath"
QT_MOC_LITERAL(39, 493, 13), // "existsInCloud"
QT_MOC_LITERAL(40, 507, 14) // "onRefreshTimer"

    },
    "f2c_cpp::CloudUploadDialog\0uploadActive\0"
    "\0active\0refreshSections\0onSelectAll\0"
    "onDeselectAll\0onUploadClicked\0"
    "onPauseClicked\0onCancelClicked\0"
    "onItemChanged\0QTreeWidgetItem*\0item\0"
    "column\0onCurrentItemChanged\0current\0"
    "previous\0onNetworkStatusUpdated\0"
    "NetworkStatus\0status\0onAwsValidationComplete\0"
    "AwsStatus\0onUploadProgress\0jobId\0"
    "uploaded\0total\0speedMBps\0percent\0"
    "onUploadStateChanged\0UploadState\0state\0"
    "onUploadCompleted\0success\0message\0"
    "onQueueChanged\0onGeocodeResult\0address\0"
    "onUploadVerified\0sectionPath\0existsInCloud\0"
    "onRefreshTimer"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_f2c_cpp__CloudUploadDialog[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      18,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       1,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,  104,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       4,    0,  107,    2, 0x0a /* Public */,
       5,    0,  108,    2, 0x08 /* Private */,
       6,    0,  109,    2, 0x08 /* Private */,
       7,    0,  110,    2, 0x08 /* Private */,
       8,    0,  111,    2, 0x08 /* Private */,
       9,    0,  112,    2, 0x08 /* Private */,
      10,    2,  113,    2, 0x08 /* Private */,
      14,    2,  118,    2, 0x08 /* Private */,
      17,    1,  123,    2, 0x08 /* Private */,
      20,    1,  126,    2, 0x08 /* Private */,
      22,    5,  129,    2, 0x08 /* Private */,
      28,    2,  140,    2, 0x08 /* Private */,
      31,    3,  145,    2, 0x08 /* Private */,
      34,    0,  152,    2, 0x08 /* Private */,
      35,    1,  153,    2, 0x08 /* Private */,
      37,    2,  156,    2, 0x08 /* Private */,
      40,    0,  161,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::Bool,    3,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 11, QMetaType::Int,   12,   13,
    QMetaType::Void, 0x80000000 | 11, 0x80000000 | 11,   15,   16,
    QMetaType::Void, 0x80000000 | 18,   19,
    QMetaType::Void, 0x80000000 | 21,   19,
    QMetaType::Void, QMetaType::Int, QMetaType::LongLong, QMetaType::LongLong, QMetaType::Double, QMetaType::Int,   23,   24,   25,   26,   27,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 29,   23,   30,
    QMetaType::Void, QMetaType::Int, QMetaType::Bool, QMetaType::QString,   23,   32,   33,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   36,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool,   38,   39,
    QMetaType::Void,

       0        // eod
};

void f2c_cpp::CloudUploadDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<CloudUploadDialog *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->uploadActive((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 1: _t->refreshSections(); break;
        case 2: _t->onSelectAll(); break;
        case 3: _t->onDeselectAll(); break;
        case 4: _t->onUploadClicked(); break;
        case 5: _t->onPauseClicked(); break;
        case 6: _t->onCancelClicked(); break;
        case 7: _t->onItemChanged((*reinterpret_cast< QTreeWidgetItem*(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 8: _t->onCurrentItemChanged((*reinterpret_cast< QTreeWidgetItem*(*)>(_a[1])),(*reinterpret_cast< QTreeWidgetItem*(*)>(_a[2]))); break;
        case 9: _t->onNetworkStatusUpdated((*reinterpret_cast< const NetworkStatus(*)>(_a[1]))); break;
        case 10: _t->onAwsValidationComplete((*reinterpret_cast< const AwsStatus(*)>(_a[1]))); break;
        case 11: _t->onUploadProgress((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< qint64(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3])),(*reinterpret_cast< double(*)>(_a[4])),(*reinterpret_cast< int(*)>(_a[5]))); break;
        case 12: _t->onUploadStateChanged((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< UploadState(*)>(_a[2]))); break;
        case 13: _t->onUploadCompleted((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 14: _t->onQueueChanged(); break;
        case 15: _t->onGeocodeResult((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 16: _t->onUploadVerified((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2]))); break;
        case 17: _t->onRefreshTimer(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (CloudUploadDialog::*)(bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CloudUploadDialog::uploadActive)) {
                *result = 0;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject f2c_cpp::CloudUploadDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_meta_stringdata_f2c_cpp__CloudUploadDialog.data,
    qt_meta_data_f2c_cpp__CloudUploadDialog,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *f2c_cpp::CloudUploadDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *f2c_cpp::CloudUploadDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_f2c_cpp__CloudUploadDialog.stringdata0))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int f2c_cpp::CloudUploadDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDialog::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 18)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 18;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 18)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 18;
    }
    return _id;
}

// SIGNAL 0
void f2c_cpp::CloudUploadDialog::uploadActive(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
