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
#include <google/spanner/v1/result_set.pb.h>
#include <google/spanner/v1/spanner.pb.h>

namespace google {
namespace cloud {
namespace spanner {
inline namespace SPANNER_CLIENT_NS {
namespace {
optional<Timestamp> ReadTimeStamp(internal::ResultSetSource const& source) {
  auto metadata = source.Metadata();
  if (metadata.has_value() && metadata->has_transaction() &&
      metadata->transaction().has_read_timestamp()) {
    return internal::FromProto(metadata->transaction().read_timestamp());
  }
  return optional<Timestamp>();
}
}  // namespace
optional<Timestamp> ResultSet::ReadTimestamp() const {
  return ReadTimeStamp(*source_);
}

optional<Timestamp> SqlResultSet::ReadTimestamp() const {
  return ReadTimeStamp(*source_);
}

optional<int64_t> SqlResultSet::GetRowsModified() const {
  if (source_->Stats()) {
    switch (source_->Stats()->row_count_case()) {
      case google::spanner::v1::ResultSetStats::kRowCountExact:
        return source_->Stats()->row_count_exact();
      case google::spanner::v1::ResultSetStats::kRowCountLowerBound:
        return source_->Stats()->row_count_lower_bound();
      case google::spanner::v1::ResultSetStats::ROW_COUNT_NOT_SET:
        return {};
    }
  }
  return {};
}

optional<std::unordered_map<std::string, std::string>> SqlResultSet::GetQueryStats() const {
  if (source_->Stats()) {

  }
  return {};
}

optional<QueryPlan> SqlResultSet::GetQueryPlan() const {
  if (source_->Stats()) {

  }
  return {};
}

}  // namespace SPANNER_CLIENT_NS
}  // namespace spanner
}  // namespace cloud
}  // namespace google

