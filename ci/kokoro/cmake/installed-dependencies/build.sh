#!/usr/bin/env bash
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

set -eu

export CC=gcc
export CXX=g++
export DISTRO=fedora
export DISTRO_VERSION=30

if [[ "${BUILD_NAME+x}" != "x" ]]; then
 echo "The BUILD_NAME is not defined or is empty. Fix the Kokoro .cfg file."
 exit 1
elif [[ "${BUILD_NAME}" = "fedora" ]]; then
  # This is the default build, nothing to configure.
  /bin/true
else
  echo "Unknown BUILD_NAME (${BUILD_NAME}). Fix the Kokoro .cfg file."
  exit 1
fi

readonly CMDDIR="$(dirname "$0")"
if [[ -z "${PROJECT_ROOT+x}" ]]; then
  readonly PROJECT_ROOT="$(cd "${CMDDIR}/../../../.."; pwd)"
fi
source "${PROJECT_ROOT}/ci/kokoro/linux-config.sh"
source "${PROJECT_ROOT}/ci/define-dump-log.sh"

echo "================================================================"
NCPU=$(nproc)
export NCPU
cd "${PROJECT_ROOT}"
echo "Building with ${NCPU} cores $(date) on ${PWD}."


echo "================================================================"
echo "Capture Docker version to troubleshoot problems $(date)."
sudo docker version
echo "================================================================"

echo "================================================================"
echo "Creating Docker image with all the development tools $(date)."
# We do not want to print the log unless there is an error. If we leave `-e`
# turned on we never get a chance to print the log, so disable it.
set +e
mkdir -p "${BUILD_OUTPUT}"
"${PROJECT_ROOT}/ci/install-retry.sh" \
  "${CMDDIR}/create-docker-image.sh" \
      >"${BUILD_OUTPUT}/create-build-docker-image.log" 2>&1 </dev/null
if [[ "$?" != 0 ]]; then
  dump_log "${BUILD_OUTPUT}/create-build-docker-image.log"
  exit 1
fi
set -e
echo "Docker image created $(date)."
sudo docker image ls
echo "================================================================"

echo "================================================================"
echo "Running the full build $(date)."
# Disable
set +e
# When running on Travis (or the command-line) the build gets a tty, and the
# build scripts can produce nicer output in that case, but on Kokoro the script
# does not get a tty, and Docker terminates the program if we pass the `-i`
# flag. Configure the right option depending on the environment.
interactive_flag="-t"
if [[ -t 0 ]]; then
  interactive_flag="-it"
fi

# Make sure the user has a $HOME directory inside the Docker container.
mkdir -p "${BUILD_HOME}"

sudo docker run \
     --cap-add SYS_PTRACE \
     --env DISTRO="${DISTRO}" \
     --env DISTRO_VERSION="${DISTRO_VERSION}" \
     --env CXX="${CXX}" \
     --env CC="${CC}" \
     --env NCPU="${NCPU:-2}" \
     --env CHECK_STYLE="${CHECK_STYLE:-}" \
     --env BAZEL_CONFIG="${BAZEL_CONFIG:-}" \
     --env RUN_INTEGRATION_TESTS="${RUN_INTEGRATION_TESTS:-}" \
     --env TERM="${TERM:-dumb}" \
     --env HOME="/v/${BUILD_HOME}" \
     --env USER="${USER}" \
     --user "${UID:-0}" \
     --volume "${PWD}":/v \
     --volume "${KOKORO_GFILE_DIR:-/dev/shm}":/c \
     --workdir /v \
     "${IMAGE}:tip" \
     "/v/ci/kokoro/cmake/installed-dependencies/build-in-docker.sh"

exit_status=$?
echo "Build finished with ${exit_status} exit status $(date)."
echo "================================================================"

if [[ "${exit_status}" != 0 ]]; then
  echo "================================================================"
  echo "Build failed, printing out logs $(date)."
  "${PROJECT_ROOT}/ci/kokoro/dump-logs.sh"
  echo "================================================================"
fi

exit ${exit_status}