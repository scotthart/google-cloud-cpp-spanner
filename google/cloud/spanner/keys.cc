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

#include "google/cloud/spanner/keys.h"
#include "google/cloud/spanner/value.h"
#include <google/protobuf/util/message_differencer.h>
#include <google/spanner/v1/keys.pb.h>

namespace google {
namespace cloud {
namespace spanner {
inline namespace SPANNER_CLIENT_NS {

bool operator==(KeySet const& lhs, KeySet const& rhs) {
  //    std::string delta;
  google::protobuf::util::MessageDifferencer differencer;
  //    differencer.ReportDifferencesToString(&delta);
  auto const result = differencer.Compare(lhs.proto_, rhs.proto_);
  return result;
}

bool operator!=(KeySet const& lhs, KeySet const& rhs) { return !(lhs == rhs); }

namespace internal {

::google::spanner::v1::KeySet ToProto(KeySet keyset) {
  return std::move(keyset.proto_);
}

}  // namespace internal
}  // namespace SPANNER_CLIENT_NS
}  // namespace spanner
}  // namespace cloud
}  // namespace google