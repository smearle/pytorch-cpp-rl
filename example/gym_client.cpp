#include <string.h>
#include <chrono>
#include <fstream>

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <cpprl/spaces.h>
#include <cpprl/storage.h>
#include <cpprl/algorithms/a2c.h>
#include <cpprl/model/mlp_base.h>
#include <cpprl/model/policy.h>

#include "communicator.h"
#include "requests.h"

using namespace gym_client;
using namespace cpprl;

// Algorithm hyperparameters
const int batch_size = 5;
const float discount_factor = 0.99;
const float entropy_coef = 1e-3;
const float learning_rate = 1e-3;
const int reward_average_window_size = 10;
const bool use_gae = true;
const float value_loss_coef = 0.5;

// Environment hyperparameters
const std::string env_name = "LunarLander-v2";
const int num_envs = 8;
const float env_gamma = discount_factor; // Set to -1 to disable

// Model hyperparameters
const int actions = 4;
const int observation_size = 8;
const int hidden_size = 64;

template <typename T>
std::vector<T> flatten_2d_vector(std::vector<std::vector<T>> const &input)
{
    std::vector<T> output;

    for (auto const &sub_vector : input)
    {
        output.reserve(output.size() + sub_vector.size());
        output.insert(output.end(), sub_vector.cbegin(), sub_vector.cend());
    }

    return output;
}

int main(int argc, char *argv[])
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("%^[%T %7l] %v%$");

    torch::set_num_threads(1);
    torch::manual_seed(0);

    spdlog::info("Connecting to gym server");
    Communicator communicator("tcp://127.0.0.1:10201");

    spdlog::info("Creating environment");
    auto make_param = std::make_shared<MakeParam>();
    make_param->env_name = env_name;
    make_param->gamma = env_gamma;
    make_param->num_envs = num_envs;
    Request<MakeParam> make_request("make", make_param);
    communicator.send_request(make_request);
    spdlog::info(communicator.get_response<MakeResponse>()->result);

    Request<InfoParam> info_request("info", std::make_shared<InfoParam>());
    communicator.send_request(info_request);
    auto env_info = communicator.get_response<InfoResponse>();
    spdlog::info("Action space: {} - [{}]", env_info->action_space_type,
                 env_info->action_space_shape);
    spdlog::info("Observation space: {} - [{}]", env_info->observation_space_type,
                 env_info->observation_space_shape);

    spdlog::info("Resetting environment");
    auto reset_param = std::make_shared<ResetParam>();
    Request<ResetParam> reset_request("reset", reset_param);
    communicator.send_request(reset_request);
    auto observation_vec = flatten_2d_vector<float>(communicator.get_response<ResetResponse>()->observation);
    auto observation = torch::from_blob(observation_vec.data(), {num_envs, observation_size});

    auto base = std::make_shared<MlpBase>(observation_size, false, hidden_size);
    ActionSpace space{"Discrete", {actions}};
    Policy policy(space, base);
    RolloutStorage storage(batch_size, num_envs, {observation_size}, space, hidden_size);
    A2C a2c(policy, value_loss_coef, entropy_coef, learning_rate);

    storage.set_first_observation(observation);

    // std::ifstream weights_file{"/home/px046/prog/pytorch-cpp-rl/build/weights.json"};
    // auto json = nlohmann::json::parse(weights_file);
    // for (const auto &parameter : json.items())
    // {
    //     if (base->named_parameters().contains(parameter.key()))
    //     {
    //         std::vector<int64_t> tensor_size = parameter.value()[0];
    //         std::vector<float> parameter_vec;
    //         if (parameter.key().find("bias") == std::string::npos)
    //         {
    //             std::vector<std::vector<float>> parameter_2d_vec = parameter.value()[1].get<std::vector<std::vector<float>>>();
    //             parameter_vec = flatten_2d_vector<float>(parameter_2d_vec);
    //         }
    //         else
    //         {
    //             parameter_vec = parameter.value()[1].get<std::vector<float>>();
    //         }
    //         NoGradGuard guard;
    //         auto json_weights = torch::from_blob(parameter_vec.data(), tensor_size).contiguous();
    //         base->named_parameters()[parameter.key()].copy_(json_weights);
    //         spdlog::info("Wrote {}", parameter.key());
    //         if (parameter.key().find("bias") == std::string::npos)
    //         {
    //             spdlog::info("Json: {} - Memory: {}", parameter.value()[1][0][0], base->named_parameters()[parameter.key()][0][0].item().toFloat());
    //         }
    //     }
    //     else if (policy->named_modules()["output"]->named_parameters().contains(parameter.key()))
    //     {
    //         std::vector<int64_t> tensor_size = parameter.value()[0];
    //         std::vector<float> parameter_vec;
    //         if (parameter.key().find("bias") == std::string::npos)
    //         {
    //             std::vector<std::vector<float>> parameter_2d_vec = parameter.value()[1].get<std::vector<std::vector<float>>>();
    //             parameter_vec = flatten_2d_vector<float>(parameter_2d_vec);
    //         }
    //         else
    //         {
    //             parameter_vec = parameter.value()[1].get<std::vector<float>>();
    //         }
    //         NoGradGuard guard;
    //         auto json_weights = torch::from_blob(parameter_vec.data(), tensor_size).contiguous();
    //         policy->named_modules()["output"]->named_parameters()[parameter.key()].copy_(json_weights);
    //         spdlog::info("Wrote {}", parameter.key());
    //         if (parameter.key().find("bias") == std::string::npos)
    //         {
    //             spdlog::info("Json: {} - Memory: {}",
    //                          parameter.value()[1][0][0],
    //                          policy->named_modules()["output"]->named_parameters()[parameter.key()][0][0].item().toFloat());
    //         }
    //     }
    // }

    std::vector<float> running_rewards(num_envs);
    int episode_count = 0;
    std::vector<float> reward_history(reward_average_window_size);

    auto start_time = std::chrono::high_resolution_clock::now();

    torch::manual_seed(0);
    for (int update = 0; update < 100000; ++update)
    {
        for (int step = 0; step < batch_size; ++step)
        {
            std::vector<torch::Tensor> act_result;
            {
                torch::NoGradGuard no_grad;
                act_result = policy->act(observation,
                                         torch::Tensor(),
                                         torch::ones({num_envs, 1}));
            }
            long *actions_array = act_result[1].data<long>();
            std::vector<std::vector<int>> actions(num_envs);
            for (int i = 0; i < num_envs; ++i)
            {
                actions[i] = {static_cast<int>(actions_array[i])};
            }

            auto step_param = std::make_shared<StepParam>();
            step_param->actions = actions;
            step_param->render = false;
            Request<StepParam> step_request("step", step_param);
            communicator.send_request(step_request);
            auto step_result = communicator.get_response<StepResponse>();
            observation_vec = flatten_2d_vector<float>(step_result->observation);
            observation = torch::from_blob(observation_vec.data(), {num_envs, observation_size});
            auto rewards = flatten_2d_vector<float>(step_result->reward);
            auto real_rewards = flatten_2d_vector<float>(step_result->real_reward);
            for (int i = 0; i < num_envs; ++i)
            {
                running_rewards[i] += real_rewards[i];
                if (step_result->done[i][0])
                {
                    reward_history[episode_count % reward_average_window_size] = running_rewards[i];
                    running_rewards[i] = 0;
                    episode_count++;
                }
            }
            auto dones = torch::zeros({num_envs, 1});
            for (int i = 0; i < num_envs; ++i)
            {
                dones[i][0] = static_cast<int>(step_result->done[i][0]);
            }

            storage.insert(observation,
                           torch::zeros({num_envs, hidden_size}),
                           act_result[1],
                           act_result[2],
                           act_result[0],
                           torch::from_blob(rewards.data(), {num_envs, 1}),
                           1 - dones);
        }

        torch::Tensor next_value;
        {
            torch::NoGradGuard no_grad;
            next_value = policy->get_values(
                                   storage.get_observations()[-1],
                                   storage.get_hidden_states()[-1],
                                   storage.get_masks()[-1])
                             .detach();
        }
        storage.compute_returns(next_value, use_gae, discount_factor, 0.9);

        auto update_data = a2c.update(storage);
        storage.after_update();

        if (update % 10 == 0)
        {
            auto total_steps = (update + 1) * batch_size * num_envs;
            auto run_time = std::chrono::high_resolution_clock::now() - start_time;
            auto run_time_secs = std::chrono::duration_cast<std::chrono::seconds>(run_time);
            auto fps = total_steps / (run_time_secs.count() + 1e-9);
            spdlog::info("---");
            spdlog::info("Update: {}", update);
            spdlog::info("FPS: {}", fps);
            for (const auto &datum : update_data)
            {
                spdlog::info("{}: {}", datum.name, datum.value);
            }
            float average_reward = std::accumulate(reward_history.begin(), reward_history.end(), 0);
            average_reward /= episode_count < reward_average_window_size ? episode_count : reward_average_window_size;
            spdlog::info("Reward: {}", average_reward);
        }
    }
}