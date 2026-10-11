#include "radio_link_pill.hpp"

#include <gtest/gtest.h>

namespace {

using f2c_cpp::RadioLinkPill;
using f2c_cpp::RadioLinkSample;
using f2c_cpp::applyRadioProbe;
using f2c_cpp::classifyRadioLink;
using f2c_cpp::kRadioFreshMs;

TEST(RadioLinkPill, UnknownUntilAProbeCompletes) {
    const RadioLinkSample sample;
    EXPECT_EQ(classifyRadioLink(sample, 10'000), RadioLinkPill::Unknown);
    EXPECT_STREQ(f2c_cpp::radioLinkLabel(RadioLinkPill::Unknown), "SIGNAL —");
}

TEST(RadioLinkPill, BandsMatchTheStage4Thresholds) {
    RadioLinkSample sample;
    applyRadioProbe(&sample, true, -65, 1'000);
    EXPECT_EQ(classifyRadioLink(sample, 1'000), RadioLinkPill::Strong);
    applyRadioProbe(&sample, true, -66, 1'000);
    EXPECT_EQ(classifyRadioLink(sample, 1'000), RadioLinkPill::Weak);
    applyRadioProbe(&sample, true, -80, 1'000);
    EXPECT_EQ(classifyRadioLink(sample, 1'000), RadioLinkPill::Weak);
    applyRadioProbe(&sample, true, -81, 1'000);
    EXPECT_EQ(classifyRadioLink(sample, 1'000), RadioLinkPill::NoLink);
}

TEST(RadioLinkPill, ZeroClearsAHeldStrongReading) {
    RadioLinkSample sample;
    applyRadioProbe(&sample, true, -60, 5'000);
    applyRadioProbe(&sample, true, 0, 5'100);
    EXPECT_EQ(classifyRadioLink(sample, 5'100), RadioLinkPill::NoLink);
    EXPECT_EQ(sample.wall_ms, 0);
}

TEST(RadioLinkPill, AFailedGetHoldsAFreshReadingThenExpires) {
    RadioLinkSample sample;
    applyRadioProbe(&sample, true, -60, 10'000);
    applyRadioProbe(&sample, false, 0, 11'000);
    EXPECT_EQ(classifyRadioLink(sample, 11'000), RadioLinkPill::Strong);
    EXPECT_EQ(classifyRadioLink(sample, 10'000 + kRadioFreshMs),
              RadioLinkPill::Strong);
    EXPECT_EQ(classifyRadioLink(sample, 10'000 + kRadioFreshMs + 1),
              RadioLinkPill::NoLink);
}

TEST(RadioLinkPill, AFailureWithNoReadingIsNoLink) {
    RadioLinkSample sample;
    applyRadioProbe(&sample, false, 0, 1'000);
    EXPECT_TRUE(sample.probed);
    EXPECT_EQ(classifyRadioLink(sample, 1'000), RadioLinkPill::NoLink);
}

TEST(RadioLinkPill, NoRadioConfigIsItsOwnLabel) {
    EXPECT_STREQ(f2c_cpp::radioLinkLabel(RadioLinkPill::NoConfig),
                 "NO RADIO CONFIG");
    // Missing radio settings are not a probe result. An unprobed sample
    // stays unknown; the screen paints NoConfig from its own flag.
    const RadioLinkSample sample;
    EXPECT_EQ(classifyRadioLink(sample, 1'000), RadioLinkPill::Unknown);
}

}  // namespace
