# Copyright 2014 Cloudera, Inc.
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
#
# Client API sample executables

cmake_minimum_required(VERSION 2.8)

find_package(kuduClient REQUIRED)
include_directories(${KUDU_CLIENT_INCLUDE_DIR})

add_executable(sample sample.cc)
target_link_libraries(sample kudu_client)
