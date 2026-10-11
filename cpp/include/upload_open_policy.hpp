/**
 * @file upload_open_policy.hpp
 * @brief When the operator may open Upload Data or press Upload — no Qt.
 *
 * A running upload is the only exemption: the card click has to reopen
 * the dialog that is already uploading, including during a scan. A held
 * notice (drive removed, press Upload to resume, failed) is deliberately
 * not an input. It must not skip the scan, copy, or profile checks.
 *
 * Order is the operator's: a scan is the thing they have to finish, then
 * the stick copy, then which robot the upload is attributed to.
 */

#pragma once

namespace f2c_cpp {

enum class UploadOpenBlock {
    None = 0,
    ScanActive,
    CopyNotReady,
    ProfileInvalid,
};

struct UploadOpenInputs {
    bool uploading = false;
    bool scan_active = false;
    bool copy_ready = false;
    bool profile_valid = false;
};

constexpr UploadOpenBlock uploadOpenBlock(UploadOpenInputs in) noexcept {
    if (in.uploading) return UploadOpenBlock::None;
    if (in.scan_active) return UploadOpenBlock::ScanActive;
    if (!in.copy_ready) return UploadOpenBlock::CopyNotReady;
    if (!in.profile_valid) return UploadOpenBlock::ProfileInvalid;
    return UploadOpenBlock::None;
}

}  // namespace f2c_cpp
