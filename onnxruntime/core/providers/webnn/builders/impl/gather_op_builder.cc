// Copyright (c) Microsoft Corporation. All rights reserved.
// Copyright (c) Intel Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/common.h"
#include "core/providers/shared/utils/utils.h"
#include "core/providers/webnn/builders/helper.h"
#include "core/providers/webnn/builders/model_builder.h"
#include "core/providers/webnn/builders/op_builder_factory.h"

#include "base_op_builder.h"

namespace onnxruntime {
namespace webnn {

class GatherOpBuilder : public BaseOpBuilder {
  // Add operator related.
 private:
  Status AddToModelBuilderImpl(ModelBuilder& model_builder, const Node& node,
                               const logging::Logger& logger) const override ORT_MUST_USE_RESULT;

  // Operator support related.
  bool HasSupportedInputsImpl(const GraphViewer&, const Node& node,
                              const emscripten::val& wnn_limits, const logging::Logger& logger) const override;
};

// Add operator related.

Status GatherOpBuilder::AddToModelBuilderImpl(ModelBuilder& model_builder,
                                              const Node& node,
                                              const logging::Logger& logger) const {
  const auto& input_defs = node.InputDefs();
  std::vector<int64_t> input_shape;
  ORT_RETURN_IF_NOT(GetShape(*input_defs[0], input_shape, logger), "Cannot get shape");
  const auto rank = input_shape.size();
  NodeAttrHelper helper(node);
  const uint32_t axis = static_cast<uint32_t>(HandleNegativeAxis(helper.Get("axis", 1), rank));

  emscripten::val input = model_builder.GetOperand(input_defs[0]->Name());
  emscripten::val indices = model_builder.GetOperand(input_defs[1]->Name());
  emscripten::val options = emscripten::val::object();
  options.set("axis", axis);
  options.set("label", node.Name());
  emscripten::val output = model_builder.GetBuilder().call<emscripten::val>("gather", input, indices, options);

  model_builder.AddOperand(node.OutputDefs()[0]->Name(), std::move(output));
  return Status::OK();
}

// Operator support related.
bool GatherOpBuilder::HasSupportedInputsImpl(const GraphViewer&, const Node& node,
                                             const emscripten::val& wnn_limits, const logging::Logger& logger) const {
  const auto& input = *node.InputDefs()[0];
  const auto& indices = *node.InputDefs()[1];
  const std::string_view op_type = node.OpType();
  int32_t input_type, indices_type;

  if (!GetType(input, input_type, logger) ||
      !GetType(indices, indices_type, logger))
    return false;

  return IsDataTypeSupportedByOp(op_type, input_type, wnn_limits, "input", "data", logger) &&
         IsDataTypeSupportedByOp(op_type, indices_type, wnn_limits, "indices", "indices", logger) &&
         IsInputRankSupportedByOp(node, wnn_limits, logger);
}

void CreateGatherOpBuilder(const std::string& op_type, OpBuilderRegistrations& op_registrations) {
  op_registrations.builders.push_back(std::make_unique<GatherOpBuilder>());
  op_registrations.op_builder_map.emplace(op_type, op_registrations.builders.back().get());
}

}  // namespace webnn
}  // namespace onnxruntime
