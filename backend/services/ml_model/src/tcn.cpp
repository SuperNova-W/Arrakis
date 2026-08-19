#include "arrakis/model/tcn.hpp"

#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace arrakis::model {
namespace {

using mlx::core::array;
using mlx::core::Shape;

void require_config(const TCNConfig& config) {
    if (config.feature_count == 0 || config.sequence_length == 0 ||
        config.hidden_channels == 0 || config.kernel_size == 0 ||
        config.kernel_size % 2 == 0 || config.dilations.empty()) {
        throw std::invalid_argument{"TCN configuration is invalid"};
    }
}

void require_parameters(const TCNConfig& config, const std::vector<array>& parameters) {
    if (parameters.size() != 2 + 2 * config.dilations.size() + 2) {
        throw std::invalid_argument{"TCN parameter count does not match configuration"};
    }
}

array relu(const array& value) {
    return mlx::core::maximum(value, array(0.0F));
}

array channel_layer_norm(const array& value) {
    const auto mean = mlx::core::mean(value, 2, true);
    const auto centered = value - mean;
    const auto variance = mlx::core::mean(mlx::core::square(centered), 2, true);
    return centered / mlx::core::sqrt(variance + array(1.0e-5F));
}

array initialized_weight(const Shape& shape, const float fan_in) {
    const auto scale = std::sqrt(2.0F / fan_in);
    return mlx::core::random::normal(shape, mlx::core::float32) * scale;
}

}  // namespace

std::vector<std::string> tcn_parameter_names(const TCNConfig& config) {
    require_config(config);
    std::vector<std::string> names{"input_weight", "input_bias"};
    for (std::size_t index = 0; index < config.dilations.size(); ++index) {
        names.push_back("block_" + std::to_string(index) + "_weight");
        names.push_back("block_" + std::to_string(index) + "_bias");
    }
    names.emplace_back("head_weight");
    names.emplace_back("head_bias");
    return names;
}

mlx::core::array tcn_logits(
    const TCNConfig& config,
    const std::vector<array>& parameters,
    const array& inputs
) {
    require_config(config);
    require_parameters(config, parameters);
    if (inputs.ndim() != 3 || inputs.shape(1) != static_cast<int>(config.sequence_length) ||
        inputs.shape(2) != static_cast<int>(config.feature_count)) {
        throw std::invalid_argument{"TCN input shape does not match configuration"};
    }

    std::size_t parameter_index = 0;
    const auto input_weight = parameters[parameter_index++];
    const auto input_bias = parameters[parameter_index++];
    auto hidden = mlx::core::conv1d(inputs, input_weight, 1, 0, 1) + input_bias;
    hidden = relu(hidden);
    if (config.use_channel_normalization) hidden = channel_layer_norm(hidden);

    for (const auto dilation : config.dilations) {
        const auto residual = hidden;
        const auto left_padding = dilation * static_cast<int>(config.kernel_size - 1);
        const auto block_weight = parameters[parameter_index++];
        const auto block_bias = parameters[parameter_index++];
        // Symmetric MLX padding produces extra positions on the right. The
        // first sequence_length positions are still strictly causal because
        // every kernel window ends at the corresponding original timestep.
        const auto padded_convolution = mlx::core::conv1d(
            hidden,
            block_weight,
            1,
            left_padding,
            dilation
        ) + block_bias;
        const auto convolution_steps = mlx::core::unstack(padded_convolution, 1);
        if (convolution_steps.size() < config.sequence_length) {
            throw std::runtime_error{"TCN padded convolution output is shorter than expected"};
        }
        std::vector<array> causal_steps(
            convolution_steps.begin(),
            convolution_steps.begin() + static_cast<std::ptrdiff_t>(config.sequence_length)
        );
        const auto convolved = mlx::core::stack(causal_steps, 1);
        hidden = relu(convolved + residual);
        if (config.use_channel_normalization) hidden = channel_layer_norm(hidden);
    }

    if (hidden.ndim() != 3 || hidden.shape(1) != static_cast<int>(config.sequence_length)) {
        throw std::runtime_error{
            "TCN hidden shape is " + std::to_string(hidden.ndim()) + "x" +
            std::to_string(hidden.ndim() > 0 ? hidden.shape(0) : -1) + "x" +
            std::to_string(hidden.ndim() > 1 ? hidden.shape(1) : -1) + "x" +
            std::to_string(hidden.ndim() > 2 ? hidden.shape(2) : -1)
        };
    }

    const auto hidden_steps = mlx::core::unstack(hidden, 1);
    if (hidden_steps.empty()) {
        throw std::runtime_error{"TCN hidden sequence is empty"};
    }
    const auto last = hidden_steps.back();
    const auto head_weight = parameters[parameter_index++];
    const auto head_bias = parameters[parameter_index++];
    const auto logits = mlx::core::matmul(last, head_weight) + head_bias;
    return mlx::core::reshape(logits, Shape{inputs.shape(0)});
}

TCNNetwork::TCNNetwork(TCNConfig config, std::vector<array> parameters)
    : config_(std::move(config)), parameters_(std::move(parameters)) {
    require_config(config_);
    require_parameters(config_, parameters_);
}

TCNNetwork TCNNetwork::random(TCNConfig config, const std::uint64_t seed) {
    require_config(config);
    mlx::core::random::seed(seed);

    std::vector<array> parameters;
    parameters.reserve(2 + 2 * config.dilations.size() + 2);
    parameters.push_back(initialized_weight(
        Shape{static_cast<int>(config.hidden_channels), 1, static_cast<int>(config.feature_count)},
        static_cast<float>(config.feature_count)
    ));
    parameters.push_back(mlx::core::zeros(Shape{static_cast<int>(config.hidden_channels)}));
    for (std::size_t index = 0; index < config.dilations.size(); ++index) {
        parameters.push_back(initialized_weight(
            Shape{static_cast<int>(config.hidden_channels), static_cast<int>(config.kernel_size), static_cast<int>(config.hidden_channels)},
            static_cast<float>(config.hidden_channels * config.kernel_size)
        ));
        parameters.push_back(mlx::core::zeros(Shape{static_cast<int>(config.hidden_channels)}));
    }
    parameters.push_back(initialized_weight(
        Shape{static_cast<int>(config.hidden_channels), 1},
        static_cast<float>(config.hidden_channels)
    ));
    parameters.push_back(mlx::core::zeros(Shape{1}));
    mlx::core::eval(parameters);
    return TCNNetwork{std::move(config), std::move(parameters)};
}

TCNNetwork TCNNetwork::load(
    TCNConfig config,
    const std::filesystem::path& weights_path
) {
    require_config(config);
    if (!std::filesystem::exists(weights_path)) {
        throw std::runtime_error{"TCN weights are missing: " + weights_path.string()};
    }
    const auto loaded = mlx::core::load_safetensors(weights_path.string());
    const auto names = tcn_parameter_names(config);
    std::vector<array> parameters;
    parameters.reserve(names.size());
    for (const auto& name : names) {
        const auto found = loaded.first.find(name);
        if (found == loaded.first.end()) {
            throw std::runtime_error{"TCN weights are missing parameter: " + name};
        }
        parameters.push_back(found->second);
    }
    mlx::core::eval(parameters);
    return TCNNetwork{std::move(config), std::move(parameters)};
}

array TCNNetwork::logits(const array& inputs) const {
    return tcn_logits(config_, parameters_, inputs);
}

array TCNNetwork::probabilities(const array& inputs) const {
    return mlx::core::sigmoid(logits(inputs));
}

void TCNNetwork::save(const std::filesystem::path& weights_path) const {
    std::filesystem::create_directories(weights_path.parent_path());
    mlx::core::eval(parameters_);
    std::unordered_map<std::string, array> weights;
    const auto names = tcn_parameter_names(config_);
    for (std::size_t index = 0; index < names.size(); ++index) {
        weights.emplace(names[index], parameters_[index]);
    }
    mlx::core::save_safetensors(weights_path.string(), std::move(weights));
}

}  // namespace arrakis::model
