#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <onnx/onnx-ml.pb.h>

namespace {

onnx::NodeProto* add_node(
    onnx::GraphProto* graph,
    const std::string& op_type,
    const std::string& name,
    const std::vector<std::string>& inputs,
    const std::string& output
) {
    auto* node = graph->add_node();
    node->set_op_type(op_type);
    node->set_name(name);
    for (const auto& input : inputs) node->add_input(input);
    node->add_output(output);
    return node;
}

void add_int_attribute(onnx::NodeProto* node, const std::string& name, const std::int64_t value) {
    auto* attribute = node->add_attribute();
    attribute->set_name(name);
    attribute->set_type(onnx::AttributeProto::INT);
    attribute->set_i(value);
}

void add_ints_attribute(
    onnx::NodeProto* node,
    const std::string& name,
    const std::vector<std::int64_t>& values
) {
    auto* attribute = node->add_attribute();
    attribute->set_name(name);
    attribute->set_type(onnx::AttributeProto::INTS);
    for (const auto value : values) attribute->add_ints(value);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: add_onnx_pooled_output <input.onnx> <output.onnx>\n";
        return 2;
    }
    onnx::ModelProto model;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input || !model.ParseFromIstream(&input)) {
        std::cerr << "could not parse input model\n";
        return 3;
    }
    auto* graph = model.mutable_graph();
    constexpr const char* final_hidden = "/bert/encoder/layer.11/output/LayerNorm/Add_1_output_0";
    constexpr const char* attention_mask = "attention_mask";
    constexpr const char* mask_float = "arrakis_mask_float";
    constexpr const char* mask_3d = "arrakis_mask_3d";
    constexpr const char* masked_hidden = "arrakis_masked_hidden";
    constexpr const char* summed_hidden = "arrakis_summed_hidden";
    constexpr const char* token_count = "arrakis_token_count";
    constexpr const char* token_count_safe = "arrakis_token_count_safe";
    constexpr const char* pooled_embedding = "pooled_embedding";
    constexpr const char* epsilon = "arrakis_pool_epsilon";

    auto* cast = add_node(graph, "Cast", "arrakis/embedding/Cast", {attention_mask}, mask_float);
    add_int_attribute(cast, "to", onnx::TensorProto::FLOAT);
    auto* unsqueeze = add_node(
        graph, "Unsqueeze", "arrakis/embedding/Unsqueeze", {mask_float}, mask_3d
    );
    add_ints_attribute(unsqueeze, "axes", {2});
    add_node(graph, "Mul", "arrakis/embedding/Mul", {final_hidden, mask_3d}, masked_hidden);
    auto* sum_hidden = add_node(
        graph, "ReduceSum", "arrakis/embedding/ReduceSumHidden", {masked_hidden}, summed_hidden
    );
    add_ints_attribute(sum_hidden, "axes", {1});
    add_int_attribute(sum_hidden, "keepdims", 0);
    auto* sum_mask = add_node(
        graph, "ReduceSum", "arrakis/embedding/ReduceSumMask", {mask_3d}, token_count
    );
    add_ints_attribute(sum_mask, "axes", {1});
    add_int_attribute(sum_mask, "keepdims", 0);
    add_node(
        graph, "Add", "arrakis/embedding/AddEpsilon", {token_count, epsilon}, token_count_safe
    );
    add_node(
        graph, "Div", "arrakis/embedding/MeanPool", {summed_hidden, token_count_safe}, pooled_embedding
    );

    auto* epsilon_tensor = graph->add_initializer();
    epsilon_tensor->set_name(epsilon);
    epsilon_tensor->set_data_type(onnx::TensorProto::FLOAT);
    epsilon_tensor->add_dims(1);
    epsilon_tensor->add_float_data(1.0e-6F);

    auto* output = graph->add_output();
    output->set_name(pooled_embedding);
    auto* tensor_type = output->mutable_type()->mutable_tensor_type();
    tensor_type->set_elem_type(onnx::TensorProto::FLOAT);
    auto* shape = tensor_type->mutable_shape();
    shape->add_dim()->set_dim_param("batch");
    shape->add_dim()->set_dim_value(768);

    std::ofstream result(argv[2], std::ios::binary);
    if (!result || !model.SerializeToOstream(&result)) {
        std::cerr << "could not write output model\n";
        return 4;
    }
    std::cout << "added pooled_embedding output to " << argv[2] << '\n';
    return 0;
}
