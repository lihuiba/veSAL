# Copyright (c) 2023 ByteDance Inc.
#
# This file is part of veSAL.
#
# veSAL is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# veSAL is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with veSAL. If not, see <https://www.gnu.org/licenses/>.

add_executable(perf_simple perf_simple.cc traffic_limiter.cc)

target_link_libraries(perf_simple PRIVATE vesal_compile_options vesal
  tcmalloc-static gflags vesal_perf_includes)

add_executable(shared_codec shared_codec.cc)
target_link_libraries(shared_codec PRIVATE vesal_compile_options vesal
  tcmalloc-static gflags vesal_perf_includes)
if(${VESAL_ENABLE_ZSTD})
  add_executable(zstd_perf zstd_perf.cc)
  target_link_libraries(zstd_perf PRIVATE vesal vesal_third_libs
    tcmalloc-static vesal_perf_includes)
endif(TRUE)

