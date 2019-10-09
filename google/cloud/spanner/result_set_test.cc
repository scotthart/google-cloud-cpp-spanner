// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/spanner/result_set.h"
#include "google/cloud/spanner/internal/time.h"
#include "google/cloud/spanner/mocks/mock_spanner_connection.h"
#include "google/cloud/spanner/testing/matchers.h"
#include "google/cloud/spanner/timestamp.h"
#include "google/cloud/internal/make_unique.h"
#include <google/protobuf/text_format.h>
#include <gmock/gmock.h>
#include <chrono>
#include <iostream>
#include <string>

namespace google {
namespace cloud {
namespace spanner {
inline namespace SPANNER_CLIENT_NS {
namespace {

namespace spanner_proto = ::google::spanner::v1;

using ::google::cloud::internal::make_unique;
using ::google::cloud::spanner_mocks::MockResultSetSource;
using ::google::protobuf::TextFormat;
using ::testing::Eq;
using ::testing::Return;
using ::testing::UnorderedPointwise;

TEST(QueryResult, IterateNoRows) {
  auto mock_source = make_unique<MockResultSetSource>();
  EXPECT_CALL(*mock_source, NextValue()).WillOnce(Return(optional<Value>()));

  QueryResult result_set(std::move(mock_source));
  int num_rows = 0;
  for (auto const& row : result_set.Rows<Row<bool>>()) {
    static_cast<void>(row);
    ++num_rows;
  }
  EXPECT_EQ(num_rows, 0);
}

TEST(QueryResult, IterateOverRows) {
  auto mock_source = make_unique<MockResultSetSource>();
  EXPECT_CALL(*mock_source, NextValue())
      .WillOnce(Return(optional<Value>(5)))
      .WillOnce(Return(optional<Value>(true)))
      .WillOnce(Return(optional<Value>("foo")))
      .WillOnce(Return(optional<Value>(10)))
      .WillOnce(Return(optional<Value>(false)))
      .WillOnce(Return(optional<Value>("bar")))
      .WillOnce(Return(optional<Value>()));

  QueryResult result_set(std::move(mock_source));
  int num_rows = 0;
  for (auto const& row :
       result_set.Rows<Row<std::int64_t, bool, std::string>>()) {
    EXPECT_TRUE(row.ok());
    switch (num_rows++) {
      case 0:
        EXPECT_EQ(row->get<0>(), 5);
        EXPECT_EQ(row->get<1>(), true);
        EXPECT_EQ(row->get<2>(), "foo");
        break;

      case 1:
        EXPECT_EQ(row->get<0>(), 10);
        EXPECT_EQ(row->get<1>(), false);
        EXPECT_EQ(row->get<2>(), "bar");
        break;

      default:
        ADD_FAILURE() << "Unexpected row number " << num_rows;
        break;
    }
  }
  EXPECT_EQ(num_rows, 2);
}

TEST(QueryResult, IterateError) {
  auto mock_source = make_unique<MockResultSetSource>();
  EXPECT_CALL(*mock_source, NextValue())
      .WillOnce(Return(optional<Value>(5)))
      .WillOnce(Return(optional<Value>(true)))
      .WillOnce(Return(optional<Value>("foo")))
      .WillOnce(Return(optional<Value>(10)))
      .WillOnce(Return(Status(StatusCode::kUnknown, "oops")));

  QueryResult result_set(std::move(mock_source));
  int num_rows = 0;
  for (auto const& row :
       result_set.Rows<Row<std::int64_t, bool, std::string>>()) {
    switch (num_rows++) {
      case 0:
        EXPECT_TRUE(row.ok());
        EXPECT_EQ(row->get<0>(), 5);
        EXPECT_EQ(row->get<1>(), true);
        EXPECT_EQ(row->get<2>(), "foo");
        break;

      case 1:
        EXPECT_FALSE(row.ok());
        EXPECT_EQ(row.status().code(), StatusCode::kUnknown);
        EXPECT_EQ(row.status().message(), "oops");
        break;

      default:
        ADD_FAILURE() << "Unexpected row number " << num_rows;
        break;
    }
  }
  EXPECT_EQ(num_rows, 2);
}

TEST(QueryResult, TimestampNoTransaction) {
  auto mock_source = make_unique<MockResultSetSource>();
  spanner_proto::ResultSetMetadata no_transaction;
  EXPECT_CALL(*mock_source, Metadata()).WillOnce(Return(no_transaction));

  QueryResult result_set(std::move(mock_source));
  EXPECT_FALSE(result_set.ReadTimestamp().has_value());
}

TEST(QueryResult, TimestampNotPresent) {
  auto mock_source = make_unique<MockResultSetSource>();
  spanner_proto::ResultSetMetadata transaction_no_timestamp;
  transaction_no_timestamp.mutable_transaction()->set_id("dummy");
  EXPECT_CALL(*mock_source, Metadata())
      .WillOnce(Return(transaction_no_timestamp));

  QueryResult result_set(std::move(mock_source));
  EXPECT_FALSE(result_set.ReadTimestamp().has_value());
}

TEST(QueryResult, TimestampPresent) {
  auto mock_source = make_unique<MockResultSetSource>();
  spanner_proto::ResultSetMetadata transaction_with_timestamp;
  transaction_with_timestamp.mutable_transaction()->set_id("dummy2");
  Timestamp timestamp = std::chrono::system_clock::now();
  *transaction_with_timestamp.mutable_transaction()->mutable_read_timestamp() =
      internal::ToProto(timestamp);
  EXPECT_CALL(*mock_source, Metadata())
      .WillOnce(Return(transaction_with_timestamp));

  QueryResult result_set(std::move(mock_source));
  EXPECT_EQ(*result_set.ReadTimestamp(), timestamp);
}

TEST(ProfileQueryResult, TimestampPresent) {
  auto mock_source = make_unique<MockResultSetSource>();
  spanner_proto::ResultSetMetadata transaction_with_timestamp;
  transaction_with_timestamp.mutable_transaction()->set_id("dummy2");
  Timestamp timestamp = std::chrono::system_clock::now();
  *transaction_with_timestamp.mutable_transaction()->mutable_read_timestamp() =
      internal::ToProto(timestamp);
  EXPECT_CALL(*mock_source, Metadata())
      .WillOnce(Return(transaction_with_timestamp));

  ProfileQueryResult result_set(std::move(mock_source));
  EXPECT_EQ(*result_set.ReadTimestamp(), timestamp);
}

TEST(ProfileQueryResult, ExecutionStats) {
  auto mock_source = make_unique<MockResultSetSource>();
  google::spanner::v1::ResultSetStats stats;
  ASSERT_TRUE(TextFormat::ParseFromString(
      R"pb(
        query_stats {
          fields {
            key: "elapsed_time"
            value { string_value: "42 secs" }
          }
        }
      )pb",
      &stats));
  EXPECT_CALL(*mock_source, Stats()).WillOnce(Return(stats));

  std::vector<std::pair<const std::string, std::string>> expected;
  expected.emplace_back("elapsed_time", "42 secs");
  ProfileQueryResult query_result(std::move(mock_source));
  EXPECT_THAT(*query_result.ExecutionStats(),
              UnorderedPointwise(Eq(), expected));
}

TEST(ProfileQueryResult, ExecutionPlan) {
  auto mock_source = make_unique<MockResultSetSource>();
  google::spanner::v1::ResultSetStats stats;
  ASSERT_TRUE(TextFormat::ParseFromString(
      R"pb(
        query_plan { plan_nodes: { index: 42 } }
      )pb",
      &stats));
  EXPECT_CALL(*mock_source, Stats()).WillRepeatedly(Return(stats));

  ProfileQueryResult query_result(std::move(mock_source));
  EXPECT_THAT(*query_result.ExecutionPlan(),
              spanner_testing::IsProtoEqual(stats.query_plan()));
}

TEST(DmlResult, RowsModified) {
  auto mock_source = make_unique<MockResultSetSource>();
  google::spanner::v1::ResultSetStats stats;
  ASSERT_TRUE(TextFormat::ParseFromString(
      R"pb(
        row_count_exact: 42
      )pb",
      &stats));
  EXPECT_CALL(*mock_source, Stats()).WillOnce(Return(stats));

  DmlResult dml_result(std::move(mock_source));
  EXPECT_EQ(dml_result.RowsModified(), 42);
}

TEST(ProfileDmlResult, RowsModified) {
  auto mock_source = make_unique<MockResultSetSource>();
  google::spanner::v1::ResultSetStats stats;
  ASSERT_TRUE(TextFormat::ParseFromString(
      R"pb(
        row_count_exact: 42
      )pb",
      &stats));
  EXPECT_CALL(*mock_source, Stats()).WillOnce(Return(stats));

  ProfileDmlResult dml_result(std::move(mock_source));
  EXPECT_EQ(dml_result.RowsModified(), 42);
}

TEST(ProfileDmlResult, ExecutionStats) {
  auto mock_source = make_unique<MockResultSetSource>();
  google::spanner::v1::ResultSetStats stats;
  ASSERT_TRUE(TextFormat::ParseFromString(
      R"pb(
        query_stats {
          fields {
            key: "elapsed_time"
            value { string_value: "42 secs" }
          }
        }
      )pb",
      &stats));
  EXPECT_CALL(*mock_source, Stats()).WillOnce(Return(stats));

  std::vector<std::pair<const std::string, std::string>> expected;
  expected.emplace_back("elapsed_time", "42 secs");
  ProfileDmlResult dml_result(std::move(mock_source));
  EXPECT_THAT(*dml_result.ExecutionStats(), UnorderedPointwise(Eq(), expected));
}

}  // namespace
}  // namespace SPANNER_CLIENT_NS
}  // namespace spanner
}  // namespace cloud
}  // namespace google
