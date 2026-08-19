#pragma once

#include <mlx/mlx.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace arrakis::model {

struct TCNConfig final {
    std::size_t feature_count{36};
    std::size_t sequence_length{30};
    std::size_t hidden_channels{16};
    std::size_t kernel_size{3};
    std::vector<int> dilations{1, 2, 4, 8};
    bool use_channel_normalization{true};
};

[[nodiscard]] std::vector<std::string> tcn_parameter_names(const TCNConfig& config);

[[nodiscard]] mlx::core::array tcn_logits(
    const TCNConfig& config,
    const std::vector<mlx::core::array>& parameters,
    const mlx::core::array& inputs
);

class TCNNetwork final {
public:
    explicit TCNNetwork(TCNConfig config, std::vector<mlx::core::array> parameters);

    [[nodiscard]] static TCNNetwork random(TCNConfig config, std::uint64_t seed);
    [[nodiscard]] static TCNNetwork load(
        TCNConfig config,
        const std::filesystem::path& weights_path
    );

    [[nodiscard]] const TCNConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::vector<mlx::core::array>& parameters() const noexcept {
        return parameters_;
    }
    [[nodiscard]] std::vector<mlx::core::array>& parameters() noexcept { return parameters_; }

    [[nodiscard]] mlx::core::array logits(const mlx::core::array& inputs) const;
    [[nodiscard]] mlx::core::array probabilities(const mlx::core::array& inputs) const;

    void save(const std::filesystem::path& weights_path) const;

private:
    TCNConfig config_;
    std::vector<mlx::core::array> parameters_;
};

}  // namespace arrakis::model
