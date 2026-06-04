// Unit tests for the OTA UpdateChecker's pure-function surface:
//   - isUpdateNewer()   : embedded-vs-remote SHA prefix comparison
//   - extractRemoteSha(): resolving a real commit SHA from a release's
//                         name / tag / target_commitish
//   - parseReleaseJson(): end-to-end parse of a Releases API payload
//
// Regression focus: the rolling `latest` tag makes GitHub return the default
// branch name ("main") in target_commitish, which previously made every poll
// look like a new build and re-surfaced the update banner after a clean
// install. These tests lock the name-first resolution + fail-quiet behavior.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

#include "update/update_checker.hpp"

using f2c_cpp::update::UpdateChecker;
using f2c_cpp::update::VersionInfo;

// ---------------------------------------------------------------------------
// isUpdateNewer
// ---------------------------------------------------------------------------

TEST(IsUpdateNewer, SameShortShaIsNotNewer) {
    EXPECT_FALSE(UpdateChecker::isUpdateNewer("abc1234", "abc1234"));
}

TEST(IsUpdateNewer, ShortVsMatchingFullIsNotNewer) {
    // Embedded short SHA is a prefix of the remote full SHA → same build.
    EXPECT_FALSE(UpdateChecker::isUpdateNewer(
        "abc1234", "abc1234deadbeefdeadbeefdeadbeefdeadbeef00"));
}

TEST(IsUpdateNewer, DifferentShaIsNewer) {
    EXPECT_TRUE(UpdateChecker::isUpdateNewer("abc1234", "0000000"));
}

TEST(IsUpdateNewer, CaseInsensitive) {
    EXPECT_FALSE(UpdateChecker::isUpdateNewer("ABC1234", "abc1234"));
}

TEST(IsUpdateNewer, EmptyOperandsAreNotNewer) {
    EXPECT_FALSE(UpdateChecker::isUpdateNewer("", "abc1234"));
    EXPECT_FALSE(UpdateChecker::isUpdateNewer("abc1234", ""));
}

TEST(IsUpdateNewer, UnknownEmbeddedNeverPrompts) {
    EXPECT_FALSE(UpdateChecker::isUpdateNewer("unknown", "abc1234"));
}

// ---------------------------------------------------------------------------
// extractRemoteSha
// ---------------------------------------------------------------------------

TEST(ExtractRemoteSha, PrefersShaInLatestReleaseName) {
    // The real-world `latest` payload: name carries the SHA, tag is "latest",
    // and target_commitish is the branch name "main".
    EXPECT_EQ(UpdateChecker::extractRemoteSha("Latest (abc1234)", "latest", "main"),
              QStringLiteral("abc1234"));
}

TEST(ExtractRemoteSha, ParsesImmutableVTagName) {
    EXPECT_EQ(UpdateChecker::extractRemoteSha("v-deadbee", "v-deadbee", "main"),
              QStringLiteral("deadbee"));
}

TEST(ExtractRemoteSha, FallsBackToVTagWhenNameHasNoSha) {
    EXPECT_EQ(UpdateChecker::extractRemoteSha("", "v-deadbee", "main"),
              QStringLiteral("deadbee"));
}

TEST(ExtractRemoteSha, UsesTargetCommitishOnlyWhenItIsAHexSha) {
    // Exactly 40 hex chars — a real full git SHA.
    const QString full = QStringLiteral("a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0");
    ASSERT_EQ(full.size(), 40);
    EXPECT_EQ(UpdateChecker::extractRemoteSha("", "latest", full), full);
}

TEST(ExtractRemoteSha, BranchNameCommitishYieldsEmpty) {
    // No SHA anywhere usable → empty → caller stays quiet.
    EXPECT_TRUE(
        UpdateChecker::extractRemoteSha("Latest", "latest", "main").isEmpty());
}

TEST(ExtractRemoteSha, LowercasesResult) {
    EXPECT_EQ(UpdateChecker::extractRemoteSha("Latest (ABC1234)", "latest", "main"),
              QStringLiteral("abc1234"));
}

// ---------------------------------------------------------------------------
// parseReleaseJson — end-to-end with the problematic `latest` shape
// ---------------------------------------------------------------------------

namespace {
QByteArray latestPayload(const QString& name, const QString& commitish) {
    return QStringLiteral(R"({
        "tag_name": "latest",
        "name": "%1",
        "target_commitish": "%2",
        "body": "- fixed things",
        "published_at": "2026-06-01T00:00:00Z",
        "assets": [
            {"name": "bdr-coverage-planner_1.0.0_amd64.deb",
             "browser_download_url": "https://example.test/x_amd64.deb",
             "size": 12345},
            {"name": "bdr-coverage-planner_1.0.0_amd64.deb.sha256",
             "browser_download_url": "https://example.test/x_amd64.deb.sha256"}
        ]
    })")
        .arg(name, commitish)
        .toUtf8();
}
}  // namespace

TEST(ParseReleaseJson, ResolvesShaFromNameNotBranchCommitish) {
    QString err;
    const VersionInfo info =
        UpdateChecker::parseReleaseJson(latestPayload("Latest (abc1234)", "main"), &err);

    EXPECT_TRUE(err.isEmpty());
    EXPECT_EQ(info.commitSha, QStringLiteral("abc1234"));
    EXPECT_EQ(info.downloadUrl, QStringLiteral("https://example.test/x_amd64.deb"));
    EXPECT_EQ(info.sha256Url,
              QStringLiteral("https://example.test/x_amd64.deb.sha256"));
    EXPECT_EQ(info.sizeBytes, 12345);

    // The whole point: a build already at abc1234 must NOT see itself as stale.
    EXPECT_FALSE(UpdateChecker::isUpdateNewer("abc1234", info.commitSha));
    // A genuinely older build still gets offered.
    EXPECT_TRUE(UpdateChecker::isUpdateNewer("0000000", info.commitSha));
}

TEST(ParseReleaseJson, NoUsableShaLeavesCommitShaEmpty) {
    // name without a SHA + branch commitish → empty commitSha → fail quiet.
    QString err;
    const VersionInfo info =
        UpdateChecker::parseReleaseJson(latestPayload("Latest", "main"), &err);

    EXPECT_TRUE(err.isEmpty());
    EXPECT_TRUE(info.commitSha.isEmpty());
    // Empty remote SHA must never be treated as an upgrade.
    EXPECT_FALSE(UpdateChecker::isUpdateNewer("abc1234", info.commitSha));
}
