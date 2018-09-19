# ******************************************************************************
# Copyright 2017-2018 Intel Corporation
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
# ******************************************************************************

include(ExternalProject)

set(EXTERNAL_NGRAPH_INSTALL_DIR ${EXTERNAL_INSTALL_DIR})
set(NGRAPH_CMAKE_PREFIX ext_ngraph)

# PREBUILD_NGRAPH_LIB_DIR and PREBUILD_NGRAPH_INCLUDE_DIR is used for TF integration
# - PREBUILD_NGRAPH_LIB_DIR is inside the python site-pacakges:
#   To get the path, use `python -c "import ngraph; print(ngraph)"`
# - PREBUILD_NGRAPH_INCLUDE_DIR is inside ngraph-tf's build directory
#
#   example:
#   cmake .. -DCMAKE_BUILD_TYPE=Debug \
#            -DPREBUILD_NGRAPH_INCLUDE_DIR=$HOME/repo/ngraph-tf/build/ngraph/ngraph_dist/include \
#            -DPREBUILD_NGRAPH_LIB_DIR=$HOME/repo/venvs/he3/lib/python3.5/site-packages/ngraph

if (PREBUILD_NGRAPH_LIB_DIR)
    set(NGRAPH_INCLUDE_DIR ${PREBUILD_NGRAPH_INCLUDE_DIR})
    set(NGRAPH_LIB_DIR ${PREBUILD_NGRAPH_LIB_DIR})

    # The only purpose here is to download the tests
    ExternalProject_Add(
        ext_ngraph
        GIT_REPOSITORY https://github.com/NervanaSystems/ngraph.git
        GIT_TAG he # based on v0.7.0
        PREFIX ${NGRAPH_CMAKE_PREFIX}
        UPDATE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_ALWAYS 1
    )
    ExternalProject_Get_Property(ext_ngraph source_dir)
    set(NGRAPH_TEST_DIR ${source_dir}/test)

else()
    ExternalProject_Add(
        ext_ngraph
        GIT_REPOSITORY https://github.com/NervanaSystems/ngraph.git
        GIT_TAG he # based on v0.7.0
        PREFIX ${NGRAPH_CMAKE_PREFIX}
        UPDATE_COMMAND ""
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
                -DNGRAPH_INSTALL_PREFIX=${EXTERNAL_NGRAPH_INSTALL_DIR}
                -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                -DNGRAPH_CPU_ENABLE=FALSE
                -DNGRAPH_UNIT_TEST_ENABLE=FALSE
                -DNGRAPH_TOOLS_ENABLE=FALSE
                -DCMAKE_INSTALL_MESSAGE=LAZY
        BUILD_BYPRODUCTS ${NGRAPH_CMAKE_PREFIX}
        BUILD_ALWAYS 1
    )

    ExternalProject_Get_Property(ext_ngraph source_dir)
    set(NGRAPH_INCLUDE_DIR ${EXTERNAL_NGRAPH_INSTALL_DIR}/include)
    set(NGRAPH_LIB_DIR ${EXTERNAL_NGRAPH_INSTALL_DIR}/lib)
    set(NGRAPH_TEST_DIR ${source_dir}/test)

endif() # if (PREBUILD_NGRAPH_PATH)