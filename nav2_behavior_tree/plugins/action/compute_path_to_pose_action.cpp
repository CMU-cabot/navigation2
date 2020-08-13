// Copyright (c) 2018 Intel Corporation
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

#include <memory>
#include <string>

#include "nav2_behavior_tree/plugins/action/compute_path_to_pose_action.hpp"

namespace nav2_behavior_tree
{

ComputePathToPoseAction::ComputePathToPoseAction(
  const std::string & xml_tag_name,
  const std::string & action_name,
  const BT::NodeConfiguration & conf)
: BtActionNode<nav2_msgs::action::ComputePathToPose>(xml_tag_name, action_name, conf)
{
  config().blackboard->set<rclcpp::Time>("path_last_updated", rclcpp::Time(0)); // NOLINT
}

void ComputePathToPoseAction::on_tick()
{
  getInput("goal", goal_.pose);
  getInput("planner_id", goal_.planner_id);
}

BT::NodeStatus ComputePathToPoseAction::on_success()
{
  setOutput("path", result_.result->path);
  rclcpp::Time now = node_->get_clock()->now();
  rclcpp::Time last_updated = config().blackboard->get<rclcpp::Time>("path_last_updated");

  if (last_updated.nanoseconds() != 0 && now > last_updated) {
    config().blackboard->set("path_updated", true);
  }
  config().blackboard->set("path_last_updated", now);

  return BT::NodeStatus::SUCCESS;
}

}  // namespace nav2_behavior_tree

#include "behaviortree_cpp_v3/bt_factory.h"
BT_REGISTER_NODES(factory)
{
  BT::NodeBuilder builder =
    [](const std::string & name, const BT::NodeConfiguration & config)
    {
      return std::make_unique<nav2_behavior_tree::ComputePathToPoseAction>(
        name, "compute_path_to_pose", config);
    };

  factory.registerBuilder<nav2_behavior_tree::ComputePathToPoseAction>(
    "ComputePathToPose", builder);
}
