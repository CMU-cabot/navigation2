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

#include "nav2_behavior_tree/plugins/action/follow_path_action.hpp"

namespace nav2_behavior_tree
{

FollowPathAction::FollowPathAction(
  const std::string & xml_tag_name,
  const std::string & action_name,
  const BT::NodeConfiguration & conf)
: BtActionNode<nav2_msgs::action::FollowPath>(xml_tag_name, action_name, conf)
{
  config().blackboard->set("path_updated", false);
}

void FollowPathAction::on_tick()
{
  getInput("path", goal_.path);
  getInput("controller_id", goal_.controller_id);
}

void FollowPathAction::on_wait_for_result()
{
  // Check if the goal has been updated
  if (config().blackboard->get<bool>("path_updated")) {
    // Reset the flag in the blackboard
    config().blackboard->set("path_updated", false);

    // Grab the new goal and set the flag so that we send the new goal to
    // the action server on the next loop iteration
    getInput("path", goal_.path);
    goal_updated_ = true;
  }
}

BT::NodeStatus FollowPathAction::on_success()
{
  config().blackboard->set<rclcpp::Time>("path_last_updated", rclcpp::Time(0)); // NOLINT
  return BtActionNode::on_success();
}

BT::NodeStatus FollowPathAction::on_aborted()
{
  config().blackboard->set<rclcpp::Time>("path_last_updated", rclcpp::Time(0)); // NOLINT
  return BtActionNode::on_aborted();
}

BT::NodeStatus FollowPathAction::on_cancelled()
{
  config().blackboard->set<rclcpp::Time>("path_last_updated", rclcpp::Time(0)); // NOLINT
  return BtActionNode::on_cancelled();
}

}  // namespace nav2_behavior_tree

#include "behaviortree_cpp_v3/bt_factory.h"
BT_REGISTER_NODES(factory)
{
  BT::NodeBuilder builder =
    [](const std::string & name, const BT::NodeConfiguration & config)
    {
      return std::make_unique<nav2_behavior_tree::FollowPathAction>(
        name, "follow_path", config);
    };

  factory.registerBuilder<nav2_behavior_tree::FollowPathAction>(
    "FollowPath", builder);
}
