/*
 * Copyright (c) 2023 ByteDance Inc.
 *
 * This file is part of veSAL.
 *
 * veSAL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * veSAL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with veSAL. If not, see <https://www.gnu.org/licenses/>.
 */

#include "common/timestamp.h"

#include <gtest/gtest.h>

namespace vesal {

TEST(SimpleTest, Basic) {
    EXPECT_FALSE(TimeStamp::TscNotReliable());
    uint64_t ts = TimeStamp::Now();
    uint64_t ms = TimeStamp::DurationToMs(ts);
    uint64_t us = TimeStamp::DurationToUs(ts);
    uint64_t ns = TimeStamp::DurationToNs(ts);
    EXPECT_EQ(ns / 1000, us);
    EXPECT_EQ(us / 1000, ms);

    EXPECT_LE(TimeStamp::MsToDuration(ms), ts);
    EXPECT_LE(TimeStamp::UsToDuration(us), ts);
    EXPECT_LE(TimeStamp::NsToDuration(ns), ts);

    EXPECT_LT(ts, TimeStamp::Now());
}

}  // namespace vesal
