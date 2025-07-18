// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/optimizer/qdq_transformer/clip_quantizelinear.h"

#include <limits>

#include "core/optimizer/initializer.h"
#include "core/optimizer/qdq_transformer/qdq_util.h"
#include "core/optimizer/utils.h"
#include "core/graph/graph_utils.h"

using namespace ONNX_NAMESPACE;
using namespace onnxruntime::common;
namespace onnxruntime {

static bool GetQConstantLowerUpper(const Graph& graph, const Node& node, float& lower, float& upper) {
  const auto& input_defs = node.InputDefs();

  constexpr size_t input_cnt_required = 3;
  if (input_defs.size() != input_cnt_required) {
    return false;
  }

  constexpr size_t s_idx = 1;
  const NodeArg* s_input = input_defs[s_idx];

  const ONNX_NAMESPACE::TensorProto* s_tensor_proto = graph_utils::GetConstantInitializer(graph, s_input->Name());
  if (!s_tensor_proto) {
    return false;
  }

  Initializer s_initializer(graph, *s_tensor_proto, graph.ModelPath());
  if (s_initializer.dims().size() != 0 ||
      s_initializer.data_type() != ONNX_NAMESPACE::TensorProto_DataType_FLOAT) {
    return false;
  }
  const float scale = s_initializer.data<float>()[0];

  constexpr size_t zp_idx = 2;
  const NodeArg* zp_input = input_defs[zp_idx];

  const ONNX_NAMESPACE::TensorProto* zp_tensor_proto = graph_utils::GetConstantInitializer(graph, zp_input->Name());
  if (!zp_tensor_proto) {
    return false;
  }

  Initializer zp_initializer(graph, *zp_tensor_proto, graph.ModelPath());
  if (zp_initializer.dims().size() != 0) {
    return false;
  }

  switch (zp_initializer.data_type()) {
    case ONNX_NAMESPACE::TensorProto_DataType_INT8: {
      const int8_t zero_point = zp_initializer.data<int8_t>()[0];
      lower = scale * (std::numeric_limits<int8_t>::lowest() - zero_point);
      upper = scale * (std::numeric_limits<int8_t>::max() - zero_point);
      break;
    }
    case ONNX_NAMESPACE::TensorProto_DataType_UINT8: {
      const uint8_t zero_point = zp_initializer.data<uint8_t>()[0];
      lower = scale * (std::numeric_limits<uint8_t>::lowest() - zero_point);
      upper = scale * (std::numeric_limits<uint8_t>::max() - zero_point);
      break;
    }
    case ONNX_NAMESPACE::TensorProto_DataType_INT16: {
      const int16_t zero_point = zp_initializer.data<int16_t>()[0];
      lower = scale * (std::numeric_limits<int16_t>::lowest() - zero_point);
      upper = scale * (std::numeric_limits<int16_t>::max() - zero_point);
      break;
    }
    case ONNX_NAMESPACE::TensorProto_DataType_UINT16: {
      const uint16_t zero_point = zp_initializer.data<uint16_t>()[0];
      lower = scale * (std::numeric_limits<uint16_t>::lowest() - zero_point);
      upper = scale * (std::numeric_limits<uint16_t>::max() - zero_point);
      break;
    }
    default:
      ORT_THROW("Unexpected data type for QuantizeLinear input y_zero_point of ", zp_initializer.data_type());
  }
  return true;
}

bool ClipQuantFusion::SatisfyCondition(const Graph& graph, const Node& node, const logging::Logger& /*logger*/) const {
  if (!graph_utils::IsSupportedOptypeVersionAndDomain(node, "Clip", {1, 6, 11, 12, 13}) ||
      !graph_utils::IsSupportedProvider(node, {kCpuExecutionProvider}) ||
      !optimizer_utils::CheckOutputEdges(graph, node, 1)) {
    return false;
  }

  // if Clip is followed by QuantizeLinear, it can be fused into QuantizeLinear potentially
  const auto& next_node = *node.OutputNodesBegin();
  if (!graph_utils::IsSupportedProvider(next_node, {kCpuExecutionProvider}) ||
      !QDQ::MatchQNode(next_node)) {
    return false;
  }

  return true;
}

Status ClipQuantFusion::Apply(Graph& graph, Node& node, RewriteRuleEffect& rule_effect, const logging::Logger&) const {
  float min, max;
  if (!optimizer_utils::GetClipConstantMinMax(graph, node, min, max)) {
    return Status::OK();
  }

  const Node& q_node = *graph.GetNode(node.OutputNodesBegin()->Index());

  float lower, upper;
  if (!GetQConstantLowerUpper(graph, q_node, lower, upper)) {
    return Status::OK();
  }

  constexpr float epsilon = std::numeric_limits<float>::epsilon();
  if (epsilon < min - lower || epsilon < upper - max) {
    return Status::OK();
  }

  if (graph_utils::RemoveNode(graph, node)) {
    rule_effect = RewriteRuleEffect::kRemovedCurrentNode;
  }

  return Status::OK();
}
}  // namespace onnxruntime
