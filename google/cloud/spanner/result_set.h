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

#ifndef GOOGLE_CLOUD_CPP_SPANNER_GOOGLE_CLOUD_SPANNER_RESULT_SET_H_
#define GOOGLE_CLOUD_CPP_SPANNER_GOOGLE_CLOUD_SPANNER_RESULT_SET_H_

#include "google/cloud/spanner/internal/time.h"
#include "google/cloud/spanner/row_parser.h"
#include "google/cloud/spanner/timestamp.h"
#include "google/cloud/optional.h"
#include <google/spanner/v1/spanner.pb.h>

namespace google {
namespace cloud {
namespace spanner {
inline namespace SPANNER_CLIENT_NS {

namespace internal {
class ResultSetSource {
 public:
  virtual ~ResultSetSource() = default;
  // Returns OK Status with no Value to indicate end-of-stream.
  virtual StatusOr<optional<Value>> NextValue() = 0;
  virtual optional<google::spanner::v1::ResultSetMetadata> Metadata() const = 0;
  virtual optional<google::spanner::v1::ResultSetStats> Stats() const = 0;
};
}  // namespace internal

class QueryPlan {

};

/**
 * Represents the result of a `SpannerClient::Read()` operation.
 *
 * Note that a `ResultSet` returns both the data for the operation, as a
 * single-pass, input range returned by `Rows()`.
 */
class ResultSet {
 public:
  ResultSet() = default;
  explicit ResultSet(std::unique_ptr<internal::ResultSetSource> source)
      : source_(std::move(source)) {}

  // This class is movable but not copyable.
  ResultSet(ResultSet&&) = default;
  ResultSet& operator=(ResultSet&&) = default;

  /**
   * Returns a `RowParser` which can be used to iterate the returned `Row`s.
   *
   * Since there is a single result stream for each `ResultSet` instance, users
   * should not use multiple `RowParser`s from the same `ResultSet` at the same
   * time. Doing so is not thread safe, and may result in errors or data
   * corruption.
   */
  template <typename... Ts>
  RowParser<Ts...> Rows() {
    return RowParser<Ts...>([this]() mutable { return source_->NextValue(); });
  }

  /**
   * Retrieve the timestamp at which the read occurred.
   *
   * Only available if a read-only transaction was used, and the timestamp
   * was requested by setting `return_read_timestamp` true.
   */
  optional<Timestamp> ReadTimestamp() const;

 private:
  std::unique_ptr<internal::ResultSetSource> source_;
};

/**
 * Represents the result of a `SpannerClient::ExecuteSql()` operation.
 *
 * This class encapsulates the result of a Cloud Spanner query, including all
 * DML operations, i.e., `UPDATE` and `DELETE` also return a `SqlResultSet`.
 *
 * Note that a `SqlResultSet` returns both the data for the operation, as a
 * single-pass, input range returned by `Rows()`, as well as the metadata for
 * the results, and execution statistics (if requested).
 */
class SqlResultSet {
 public:
  SqlResultSet() = default;
  explicit SqlResultSet(std::unique_ptr<internal::ResultSetSource> source)
      : source_(std::move(source)) {}

  // This class is movable but not copyable.
  SqlResultSet(SqlResultSet&&) = default;
  SqlResultSet& operator=(SqlResultSet&&) = default;

  /**
   * Returns a `RowParser` which can be used to iterate the returned `Row`s.
   *
   * Since there is a single result stream for each `ResultSet` instance, users
   * should not use multiple `RowParser`s from the same `ResultSet` at the same
   * time. Doing so is not thread safe, and may result in errors or data
   * corruption.
   */
  template <typename... Ts>
  RowParser<Ts...> Rows() {
    return RowParser<Ts...>([this]() mutable { return source_->NextValue(); });
  }

  /**
   * Retrieve the timestamp at which the read occurred.
   *
   * Only available if a read-only transaction was used, and the timestamp
   * was requested by setting `return_read_timestamp` true.
   */
  optional<Timestamp> ReadTimestamp() const;

  /**
   * Returns the number of rows modified by the DML statement.
   *
   * @note Partitioned DML only provides a lower bound of the rows modified, all
   * other DML statements provide an exact count.
   */
  optional<int64_t> GetRowsModified() const;

  /**
   * Returns a collection of key value pair statistics for the query execution.
   *
   * @note Only available when the query is profiled.
   */
  optional<std::unordered_map<std::string, std::string>> GetQueryStats() const;

  /**
   * Returns the plan of execution for the query.
   *
   * @note
   */
  optional<QueryPlan> GetQueryPlan() const;

 private:
  std::unique_ptr<internal::ResultSetSource> source_;
};


}  // namespace SPANNER_CLIENT_NS
}  // namespace spanner
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_SPANNER_GOOGLE_CLOUD_SPANNER_RESULT_SET_H_
