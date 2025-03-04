#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

#include <torch/csrc/inductor/aoti_runner/model_container_runner_cpu.h>
#ifdef USE_CUDA
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cuda.h>
#endif
#include <torch/script.h>
#include <torch/torch.h>

#define STR_VALUE(x) #x
#define STRINGIZE(x) STR_VALUE(x)

namespace {

void test_aoti(const std::string& device) {
  torch::NoGradGuard no_grad;

  std::string data_path =
      (std::filesystem::path(STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "data.pt")
           .string();
  torch::jit::script::Module data_loader = torch::jit::load(data_path);
  std::string path_attr = "model_so_path_" + device;
  std::string inputs_attr = "inputs_" + device;
  std::string outputs_attr = "outputs_" + device;
  const auto& model_so_path = data_loader.attr(path_attr.c_str()).toStringRef();
  const auto& input_tensors =
      data_loader.attr(inputs_attr.c_str()).toTensorList().vec();
  const auto& ref_output_tensors =
      data_loader.attr(outputs_attr.c_str()).toTensorList().vec();

  std::unique_ptr<torch::inductor::AOTIModelContainerRunner> runner;
  if (device == "cuda") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCuda>(
        model_so_path.c_str());
  } else if (device == "cpu") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCpu>(
        model_so_path.c_str());
  } else {
    testing::AssertionFailure() << "unsupported device: " << device;
  }
  auto actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));
}

void test_aoti_script(const std::string& device) {
  torch::NoGradGuard no_grad;

  std::string script_model = "script_model_" + device + ".pt";
  std::string model_path =
      (std::filesystem::path(
           STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / script_model.c_str())
           .string();
  torch::jit::script::Module model = torch::jit::load(model_path);

  std::string sample_data_path =
      (std::filesystem::path(
           STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "script_data.pt")
           .string();
  torch::jit::script::Module sample_data = torch::jit::load(sample_data_path);
  std::string inputs_attr = "inputs_" + device;
  std::string outputs_attr = "outputs_" + device;
  const auto& inputs = sample_data.attr(inputs_attr.c_str()).toList().vec();
  const auto& ref_output_tensors =
      sample_data.attr(outputs_attr.c_str()).toTensorVector();
  auto outputs = model.forward(inputs).toTuple()->elements();
  ASSERT_EQ(outputs.size(), ref_output_tensors.size());
  for (size_t i = 0; i < ref_output_tensors.size(); i++) {
    ASSERT_TRUE(torch::allclose(outputs[i].toTensor(), ref_output_tensors[i]));
  }
}

void test_aoti_constants_update(const std::string& device) {
  torch::NoGradGuard no_grad;

  std::string data_path =
      (std::filesystem::path(STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "data.pt")
           .string();

  torch::jit::script::Module data_loader = torch::jit::load(data_path);
  std::string path_attr = "model_so_path_" + device;
  std::string inputs_attr = "inputs_" + device;
  std::string outputs_attr = "outputs_" + device;
  std::string weights_attr = "fc_weight_" + device;
  std::string bias_attr = "fc_bias_" + device;
  const auto& model_so_path = data_loader.attr(path_attr.c_str()).toStringRef();
  const auto& input_tensors =
      data_loader.attr(inputs_attr.c_str()).toTensorList().vec();
  const auto& ref_output_tensors =
      data_loader.attr(outputs_attr.c_str()).toTensorList().vec();

  const auto& weight_tensors =
      data_loader.attr(weights_attr.c_str()).toTensor();
  const auto& bias_tensors = data_loader.attr(bias_attr.c_str()).toTensor();

  torch::inductor::TensorConstantMap missing_map, rand_map, real_map;
  missing_map.emplace(
      "L__self___fc_weight", new at::Tensor(at::randn({10, 64})));
  rand_map.emplace("L__self___fc_weight", new at::Tensor(at::randn({10, 64})));
  rand_map.emplace("L__self___fc_bias", new at::Tensor(at::randn({10})));
  real_map.emplace("L__self___fc_weight", new at::Tensor(weight_tensors));
  real_map.emplace("L__self___fc_bias", new at::Tensor(bias_tensors));

  std::unique_ptr<torch::inductor::AOTIModelContainerRunner> runner;
  if (device == "cuda") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCuda>(
        model_so_path.c_str());
  } else if (device == "cpu") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCpu>(
        model_so_path.c_str());
  } else {
    testing::AssertionFailure() << "unsupported device: " << device;
  }
  // By default, buffer #1 get loaded with burned in weights. Correct results.
  auto actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // Update with missing map which should throw.
  EXPECT_THROW(
      runner->update_constant_buffer(missing_map, false, true),
      std::runtime_error);

  // Update random weight to buffer #1.
  runner->update_constant_buffer(missing_map, false, false);
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_FALSE(
      torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // Update with real weight.
  runner->update_constant_buffer(real_map, false, false);
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // Update with full random weight.
  runner->update_constant_buffer(rand_map, false, true);
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_FALSE(
      torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));
}

void test_aoti_double_buffering(const std::string& device) {
  torch::NoGradGuard no_grad;

  std::string data_path =
      (std::filesystem::path(STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "data.pt")
           .string();

  torch::jit::script::Module data_loader = torch::jit::load(data_path);
  std::string path_attr = "model_so_path_" + device;
  std::string inputs_attr = "inputs_" + device;
  std::string outputs_attr = "outputs_" + device;
  std::string weights_attr = "fc_weight_" + device;
  std::string bias_attr = "fc_bias_" + device;
  const auto& model_so_path = data_loader.attr(path_attr.c_str()).toStringRef();
  const auto& input_tensors =
      data_loader.attr(inputs_attr.c_str()).toTensorList().vec();
  const auto& ref_output_tensors =
      data_loader.attr(outputs_attr.c_str()).toTensorList().vec();

  const auto& weight_tensors =
      data_loader.attr(weights_attr.c_str()).toTensor();
  const auto& bias_tensors = data_loader.attr(bias_attr.c_str()).toTensor();

  torch::inductor::TensorConstantMap rand_map, real_map;
  rand_map.emplace("L__self___fc_weight", new at::Tensor(at::randn({10, 64})));
  rand_map.emplace("L__self___fc_bias", new at::Tensor(at::randn({10})));
  real_map.emplace("L__self___fc_weight", new at::Tensor(weight_tensors));
  real_map.emplace("L__self___fc_bias", new at::Tensor(bias_tensors));

  std::unique_ptr<torch::inductor::AOTIModelContainerRunner> runner;
  if (device == "cuda") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCuda>(
        model_so_path.c_str());
  } else if (device == "cpu") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCpu>(
        model_so_path.c_str());
  } else {
    testing::AssertionFailure() << "unsupported device: " << device;
  }
  // By default, buffer #1 get loaded with burned in weights. Correct results.
  auto actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // We update the weights to buffer #2 and activate it. This should still
  // produce correct result, as it's the real constant map.
  runner->update_inactive_constant_buffer(real_map);
  runner->swap_constant_buffer();
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // We update random weights to buffer #1. But do not swap in the weight yet.
  runner->update_inactive_constant_buffer(rand_map);
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // We swap and activate the weight to buffer #1. This is random weight and
  // should produce in correct results.
  runner->swap_constant_buffer();
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_FALSE(
      torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // Swap back to buffer #2 which is the real constants.
  runner->swap_constant_buffer();
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));
}

} // namespace

namespace torch {
namespace inductor {

TEST(AotInductorTest, BasicTestCpu) {
  test_aoti("cpu");
}

TEST(AotInductorTest, BasicScriptTestCpu) {
  test_aoti_script("cpu");
}

#ifdef USE_CUDA
TEST(AotInductorTest, BasicTestCuda) {
  test_aoti("cuda");
}

TEST(AotInductorTest, BasicScriptTestCuda) {
  test_aoti_script("cuda");
}

TEST(AotInductorTest, UpdateConstantsCuda) {
  test_aoti_constants_update("cuda");
}

TEST(AotInductorTest, UpdateInactiveConstantsCuda) {
  test_aoti_double_buffering("cuda");
}
#endif

} // namespace inductor
} // namespace torch
