//*****************************************************************************
// Copyright 2018-2019 Intel Corporation
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
//*****************************************************************************

#include "node_wrapper.hpp"

ngraph::he::NodeWrapper::NodeWrapper(
    const std::shared_ptr<const ngraph::Node>& node)
    : m_node{node} {
// This expands the op list in op_tbl.hpp into a list of enumerations that look
// like this:
// {"Abs", ngraph::he::OP_TYPEID::Abs},
// {"Acos", ngraph::he::OP_TYPEID::Acos},
// ...
#define NGRAPH_OP(a, b) {#a, ngraph::he::OP_TYPEID::a},
  static std::unordered_map<std::string, ngraph::he::OP_TYPEID> typeid_map{
#include "ngraph/op/op_tbl.hpp"
      NGRAPH_OP(BoundedRelu, ngraph::op)};
#undef NGRAPH_OP
  auto it = typeid_map.find(m_node->description());
  if (it != typeid_map.end()) {
    m_typeid = it->second;
  } else {
    throw unsupported_op("Unsupported op '" + m_node->description() + "'");
  }
}
