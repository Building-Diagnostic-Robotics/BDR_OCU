#include "satellite_tile_service.hpp"

#include <gtest/gtest.h>

#include <QDate>

using f2c_cpp::ImageryInfo;
using f2c_cpp::TileService;

TEST(TilePolicy, MetadataLayerForZoom) {
    EXPECT_EQ(TileService::metadataLayerForZoom(19), 9);
    EXPECT_EQ(TileService::metadataLayerForZoom(20), 8);
    EXPECT_EQ(TileService::metadataLayerForZoom(21), 7);
}

TEST(TilePolicy, ImageryMeetsAgeFailsClosed) {
    ImageryInfo unknown;
    EXPECT_FALSE(TileService::imageryMeetsAge(unknown, 3, QDate(2026, 9, 9)));

    ImageryInfo recent;
    recent.valid = true;
    recent.captured = QDate(2025, 4, 1);
    EXPECT_TRUE(TileService::imageryMeetsAge(recent, 3, QDate(2026, 9, 9)));

    ImageryInfo old;
    old.valid = true;
    old.captured = QDate(2016, 3, 16);
    EXPECT_FALSE(TileService::imageryMeetsAge(old, 3, QDate(2026, 9, 9)));
}

TEST(TilePolicy, TilesForAreaNonEmpty) {
    const auto tiles = TileService::tilesForArea(29.3, -97.9, 200.0, 15, 16);
    EXPECT_FALSE(tiles.isEmpty());
}
