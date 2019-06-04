# ~~~
# Copyright 2019 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ~~~

find_package(google_cloud_cpp_common CONFIG REQUIRED)
find_package(googleapis CONFIG REQUIRED)

# TODO(#43) - generate version_info.h in the source directory and commit it.
configure_file(version_info.h.in version_info.h)
add_library(spanner_client
            ${CMAKE_BINARY_DIR}/google/cloud/spanner/version_info.h
            value.cc
            value.h
            version.cc
            version.h)
target_include_directories(spanner_client
                           PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
                                  $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
                                  $<INSTALL_INTERFACE:include>)
target_link_libraries(spanner_client
                      PUBLIC google_cloud_cpp_common
                             googleapis-c++::spanner_protos)
set_target_properties(spanner_client
                      PROPERTIES VERSION
                                 "${SPANNER_CLIENT_VERSION}"
                                 SOVERSION
                                 "${SPANNER_CLIENT_VERSION_MAJOR}")

add_library(googleapis-c++::spanner_client ALIAS spanner_client)

# To avoid maintaining the list of files for the library, export them to a .bzl
# file.
include(CreateBazelConfig)
create_bazel_config(spanner_client)
# Also export the version numbers.
#
# TODO(#43) - This might be unnecessary if we commit version_info.h instead.
export_variables_to_bazel("spanner_client_version.bzl"
                          SPANNER_CLIENT_VERSION_MAJOR
                          SPANNER_CLIENT_VERSION_MINOR
                          SPANNER_CLIENT_VERSION_PATCH)

# We need the GCS client library because we use the RFC 3339 formatting
# functions are in that library.
#
# TODO(#44) - remove the dependency on the storage client.
find_package(storage_client CONFIG REQUIRED)

add_executable(spanner_tool spanner_tool.cc)
target_link_libraries(spanner_tool
                      PRIVATE googleapis-c++::spanner_client storage_client)

# Define the tests in a function so we have a new scope for variable names.
function (spanner_client_define_tests)
    # The tests require googletest to be installed. Force CMake to use the
    # config file for googletest (that is, the CMake file installed by
    # googletest itself), because the generic `FindGTest` module does not define
    # the GTest::gmock target, and the target names are also weird.
    find_package(GTest CONFIG REQUIRED)

    set(spanner_client_unit_tests spanner_version_test.cc value_test.cc)
    # Export the list of unit tests to a .bzl file so we do not need to maintain
    # the list in two places.
    export_list_to_bazel("spanner_client_unit_tests.bzl"
                         "spanner_client_unit_tests")

    # Create a custom target so we can say "build all the tests"
    add_custom_target(spanner-client-tests)

    # Generate a target for each unit test.
    foreach (fname ${spanner_client_unit_tests})
        string(REPLACE "/"
                       "_"
                       target
                       ${fname})
        string(REPLACE ".cc"
                       ""
                       target
                       ${target})
        add_executable(${target} ${fname})
        target_link_libraries(${target}
                              PRIVATE googleapis-c++::spanner_client
                                      GTest::gmock_main
                                      GTest::gmock
                                      GTest::gtest)
        # With googletest it is relatively easy to exceed the default number of
        # sections (~65,000) in a single .obj file. Add the /bigobj option to
        # all the tests, even if it is not needed.
        if (MSVC)
            target_compile_options(${target} PRIVATE "/bigobj")
        endif ()
        add_test(NAME ${target} COMMAND ${target})
        add_dependencies(spanner-client-tests ${target})
    endforeach ()
endfunction ()

# Only define the tests if testing is enabled. Package maintainers may not want
# to build all the tests everytime they create a new package or when the package
# is installed from source.
if (BUILD_TESTING)
    spanner_client_define_tests()
endif (BUILD_TESTING)