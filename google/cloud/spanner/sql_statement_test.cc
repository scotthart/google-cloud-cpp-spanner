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

#include "google/cloud/spanner/sql_statement.h"
#include <gmock/gmock.h>

namespace google {
namespace cloud {
namespace spanner {
inline namespace SPANNER_CLIENT_NS {

using ::testing::Eq;
using ::testing::UnorderedPointwise;

TEST(SqlStatementTest, SqlAccessor) {
  const char* statement = "select * from foo";
  SqlStatement stmt(statement);
  EXPECT_EQ(statement, stmt.sql());
}

TEST(SqlStatementTest, ParamsAccessor) {
  SqlStatement::ParamType params = {{"last", Value("Blues")},
                                    {"first", Value("Elwood")}};
  SqlStatement stmt("select * from foo", params);
  EXPECT_TRUE(params == stmt.params());
}

TEST(SqlStatementTest, ParameterNames) {
  std::vector<std::string> expected = {"first", "last"};
  SqlStatement::ParamType params = {{"last", Value("Blues")},
                                    {"first", Value("Elwood")}};
  SqlStatement stmt("select * from foo", params);
  auto results = stmt.ParameterNames();
  EXPECT_THAT(expected, UnorderedPointwise(Eq(), results));
}

TEST(SqlStatementTest, GetParameterExists) {
  auto expected = Value("Elwood");
  SqlStatement::ParamType params = {{"last", Value("Blues")},
                                    {"first", Value("Elwood")}};
  SqlStatement stmt("select * from foo", params);
  auto results = stmt.GetParameter("first");
  ASSERT_TRUE(results.ok());
  EXPECT_EQ(expected, *results);
  EXPECT_EQ(std::string("Elwood"), *(results->get<std::string>()));
}

TEST(SqlStatementTest, GetParameterNotExist) {
  SqlStatement::ParamType params = {{"last", Value("Blues")},
                                    {"first", Value("Elwood")}};
  SqlStatement stmt("select * from foo", params);
  auto results = stmt.GetParameter("middle");
  ASSERT_FALSE(results.ok());
  EXPECT_EQ(StatusCode::kNotFound, results.status().code());
}

TEST(SqlStatementTest, OStreamOperatorNoParams) {
  SqlStatement s1("SELECT * FROM TABLE FOO;");
  std::stringstream ss;
  ss << s1;
  EXPECT_EQ(s1.sql(), ss.str());
}

TEST(SqlStatementTest, OStreamOperatorWithParams) {
  SqlStatement::ParamType params = {{"last", Value("Blues")},
                                    {"first", Value("Elwood")}};
  SqlStatement stmt("select * from foo", params);
  std::string expected(
      "select * from foo\n"
      "[param]: {value}\t[first]: {code: STRING; string_value: \"Elwood\"}\n"
      "[param]: {value}\t[last]: {code: STRING; string_value: \"Blues\"}");
  std::stringstream ss;
  ss << stmt;
  EXPECT_EQ(expected, ss.str());
}

}  // namespace SPANNER_CLIENT_NS
}  // namespace spanner
}  // namespace cloud
}  // namespace google
