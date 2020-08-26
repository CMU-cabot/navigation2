// Copyright (c) 2019 Intel Corporation
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

#ifndef NAV2_UTIL__SIMPLE_ACTION_SERVER_HPP_
#define NAV2_UTIL__SIMPLE_ACTION_SERVER_HPP_

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <future>
#include <chrono>
#include <algorithm>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"

namespace nav2_util
{

template<typename ActionT, typename nodeT = rclcpp::Node>
class SimpleActionServer
{
public:
  typedef std::function<void ()> ExecuteCallback;

  explicit SimpleActionServer(
    typename nodeT::SharedPtr node,
    const std::string & action_name,
    ExecuteCallback execute_callback,
    std::chrono::milliseconds server_timeout = std::chrono::milliseconds(500))
  : SimpleActionServer(
      node->get_node_base_interface(),
      node->get_node_clock_interface(),
      node->get_node_logging_interface(),
      node->get_node_waitables_interface(),
      action_name, execute_callback, server_timeout)
  {}

  explicit SimpleActionServer(
    rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base_interface,
    rclcpp::node_interfaces::NodeClockInterface::SharedPtr node_clock_interface,
    rclcpp::node_interfaces::NodeLoggingInterface::SharedPtr node_logging_interface,
    rclcpp::node_interfaces::NodeWaitablesInterface::SharedPtr node_waitables_interface,
    const std::string & action_name,
    ExecuteCallback execute_callback,
    std::chrono::milliseconds server_timeout = std::chrono::milliseconds(500))
  : node_base_interface_(node_base_interface),
    node_clock_interface_(node_clock_interface),
    node_logging_interface_(node_logging_interface),
    node_waitables_interface_(node_waitables_interface),
    action_name_(action_name),
    execute_callback_(execute_callback),
    server_timeout_(server_timeout)
  {
    using namespace std::placeholders;  // NOLINT
    action_server_ = rclcpp_action::create_server<ActionT>(
      node_base_interface_,
      node_clock_interface_,
      node_logging_interface_,
      node_waitables_interface_,
      action_name_,
      std::bind(&SimpleActionServer::handle_goal, this, _1, _2),
      std::bind(&SimpleActionServer::handle_cancel, this, _1),
      std::bind(&SimpleActionServer::handle_accepted, this, _1));
  }

  rclcpp_action::GoalResponse handle_goal(
    const rclcpp_action::GoalUUID & /*uuid*/,
    std::shared_ptr<const typename ActionT::Goal>/*goal*/)
  {
    if (!server_active_) {
      return rclcpp_action::GoalResponse::REJECT;
    }

    debug_msg("Received request for goal acceptance");
    return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
  }

  rclcpp_action::CancelResponse handle_cancel(
    const std::shared_ptr<rclcpp_action::ServerGoalHandle<ActionT>>/*handle*/)
  {
    debug_msg("Received request for goal cancellation");
    return rclcpp_action::CancelResponse::ACCEPT;
  }

  void handle_accepted(const std::shared_ptr<rclcpp_action::ServerGoalHandle<ActionT>> handle)
  {
    std::lock_guard<std::recursive_mutex> lock(update_mutex_);
    debug_msg("Receiving a new goal");
    goal_handles_queue_.push_back(handle);
    auto current_handle = goal_handles_queue_[0];
    auto size = goal_handles_queue_.size();

    if (size == 1) {
      // Return quickly to avoid blocking the executor, so spin up a new thread
      debug_msg("Executing goal asynchronously.");
      current_handle_ = current_handle;
      execution_future_ = std::async(std::launch::async, [this]() {work();});
    }
  }

  void work()
  {
    while (rclcpp::ok() && !stop_execution_ && is_active(current_handle_))
    {
      debug_msg("Executing the goal...");
      try {
        execute_callback_();
      } catch (std::exception & ex) {
        RCLCPP_ERROR(
          node_logging_interface_->get_logger(),
          "Action server failed while executing action callback: \"%s\"", ex.what());
        terminate_all();
        return;
      }

      // this should be used in the subclasses to be stopped
      if (stop_execution_) {
        warn_msg("Stopping the thread per request.");
        terminate_all();
        break;
      }

      std::unique_lock<std::recursive_mutex> lock(goal_mutex_);
      if (is_active(current_handle_)) {
        warn_msg("Current goal was not completed successfully.");
        terminate(current_handle_);
      }
      lock.unlock();

      if (is_preempt_requested()) {
	accept_pending_goal();
      }
    }

    std::unique_lock<std::recursive_mutex> lock(update_mutex_);
    erase_handle(current_handle_);
    current_handle_.reset();
    lock.unlock();
    
    debug_msg("Worker thread done.");
  }

  void activate()
  {
    std::lock_guard<std::recursive_mutex> lock(update_mutex_);
    server_active_ = true;
    stop_execution_ = false;
  }

  void deactivate()
  {
    debug_msg("Deactivating...");

    {
      std::lock_guard<std::recursive_mutex> lock(update_mutex_);
      server_active_ = false;
      stop_execution_ = true;
    }

    if (!execution_future_.valid()) {
      return;
    }

    if (is_running()) {
      warn_msg(
        "Requested to deactivate server but goal is still executing."
        " Should check if action server is running before deactivating.");
    }

    using namespace std::chrono;  //NOLINT
    auto start_time = steady_clock::now();
    while (execution_future_.wait_for(milliseconds(100)) != std::future_status::ready) {
      info_msg("Waiting for async process to finish.");
      if (steady_clock::now() - start_time >= server_timeout_) {
        terminate_all();
        throw std::runtime_error("Action callback is still running and missed deadline to stop");
      }
    }

    debug_msg("Deactivation completed.");
  }

  bool is_running()
  {
    return execution_future_.valid() &&
           (execution_future_.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::timeout);
  }

  bool is_server_active()
  {
    return server_active_;
  }

  bool is_preempt_requested() const
  {
    std::lock_guard<std::recursive_mutex> lock(update_mutex_);
    return goal_handles_queue_.size() > 1;
  }

  const std::shared_ptr<const typename ActionT::Goal> accept_pending_goal()
  {
    if (!is_preempt_requested()) {
      error_msg("Attempting to get pending goal when not available");
      return std::shared_ptr<const typename ActionT::Goal>();
    }


    do {
      std::unique_lock<std::recursive_mutex> lock(goal_mutex_);
      if (is_active(current_handle_)) {
	current_handle_->abort(empty_result());
      }
      lock.unlock();
      
      std::unique_lock<std::recursive_mutex> lock2(update_mutex_);
      erase_handle(current_handle_);
      current_handle_ = get_next_handle();
      lock2.unlock();
    } while(is_preempt_requested());

    if (!is_active(current_handle_)) {
      error_msg("Attempting to get pending goal when not available");
      return std::shared_ptr<const typename ActionT::Goal>();      
    }

    info_msg("Preempted goal");

    return current_handle_->get_goal();
  }

  const std::shared_ptr<const typename ActionT::Goal> get_current_goal() const
  {
    std::lock_guard<std::recursive_mutex> lock(update_mutex_);

    if (!is_active(current_handle_)) {
      error_msg("A goal is not available or has reached a final state");
      return std::shared_ptr<const typename ActionT::Goal>();
    }

    return current_handle_->get_goal();
  }

  bool is_cancel_requested() const
  {
    std::lock_guard<std::recursive_mutex> lock(update_mutex_);

    bool result = true;
    for(auto handle : goal_handles_queue_) {
      result = result && handle->is_canceling();
    }

    return result;
  }

  void terminate_all(
    typename std::shared_ptr<typename ActionT::Result> result =
    std::make_shared<typename ActionT::Result>())
  {

    while(current_handle_ != nullptr) {
      std::unique_lock<std::recursive_mutex> lock(goal_mutex_);
      terminate(current_handle_, result);
      lock.unlock();
      
      std::unique_lock<std::recursive_mutex> lock2(update_mutex_);
      erase_handle(current_handle_);
      current_handle_ = get_next_handle();
      lock2.unlock();
    }
  }

  void terminate_current(
    typename std::shared_ptr<typename ActionT::Result> result =
    std::make_shared<typename ActionT::Result>())
  {
    std::unique_lock<std::recursive_mutex> lock(goal_mutex_);
    terminate(current_handle_, result);
    lock.unlock();

    std::unique_lock<std::recursive_mutex> lock2(update_mutex_);
    erase_handle(current_handle_);
    current_handle_.reset();
    lock2.unlock();
  }

  void succeeded_current(
    typename std::shared_ptr<typename ActionT::Result> result =
    std::make_shared<typename ActionT::Result>())
  {
    std::unique_lock<std::recursive_mutex> lock(goal_mutex_);
    if (is_active(current_handle_)) {
      debug_msg("Setting succeed on current goal.");
      current_handle_->succeed(result);
    }
    lock.unlock();
    
    std::unique_lock<std::recursive_mutex> lock2(update_mutex_);
    erase_handle(current_handle_);
    current_handle_.reset();
    lock2.unlock();
  }

  void publish_feedback(typename std::shared_ptr<typename ActionT::Feedback> feedback)
  {
    std::lock_guard<std::recursive_mutex> lock(goal_mutex_);

    if (!is_active(current_handle_)) {
      error_msg("Trying to publish feedback when the current goal handle is not active");
      return;
    }

    current_handle_->publish_feedback(feedback);
  }

protected:
  // The SimpleActionServer isn't itself a node, so it needs interfaces to one
  rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base_interface_;
  rclcpp::node_interfaces::NodeClockInterface::SharedPtr node_clock_interface_;
  rclcpp::node_interfaces::NodeLoggingInterface::SharedPtr node_logging_interface_;
  rclcpp::node_interfaces::NodeWaitablesInterface::SharedPtr node_waitables_interface_;
  std::string action_name_;

  ExecuteCallback execute_callback_;
  std::future<void> execution_future_;
  bool stop_execution_{false};

  mutable std::recursive_mutex update_mutex_;
  mutable std::recursive_mutex goal_mutex_;
  bool server_active_{false};
  std::chrono::milliseconds server_timeout_;

  
  std::shared_ptr<rclcpp_action::ServerGoalHandle<ActionT>> current_handle_;
  std::vector<std::shared_ptr<rclcpp_action::ServerGoalHandle<ActionT>>> goal_handles_queue_;

  typename rclcpp_action::Server<ActionT>::SharedPtr action_server_;

  constexpr auto empty_result() const
  {
    return std::make_shared<typename ActionT::Result>();
  }

  constexpr std::shared_ptr<rclcpp_action::ServerGoalHandle<ActionT>> get_next_handle() const
  {    
    std::lock_guard<std::recursive_mutex> lock(update_mutex_);
    if (goal_handles_queue_.size() == 0) {
      return nullptr;
    }

    return goal_handles_queue_[0];
  }

  void erase_handle(const std::shared_ptr<rclcpp_action::ServerGoalHandle<ActionT>> handle)
  {    
    std::lock_guard<std::recursive_mutex> lock(update_mutex_);

    if (goal_handles_queue_[0] == handle) {
      goal_handles_queue_.erase(goal_handles_queue_.begin());
    }
  }

  constexpr bool is_active(
    const std::shared_ptr<rclcpp_action::ServerGoalHandle<ActionT>> handle) const
  {
    return handle != nullptr && handle->is_active();
  }

  void terminate(
    std::shared_ptr<rclcpp_action::ServerGoalHandle<ActionT>> handle,
    typename std::shared_ptr<typename ActionT::Result> result =
    std::make_shared<typename ActionT::Result>())
  {
    std::lock_guard<std::recursive_mutex> lock(update_mutex_);

    if (is_active(handle)) {
      if (handle->is_canceling()) {
        warn_msg("Client requested to cancel the goal. Cancelling.");
        handle->canceled(result);
      } else {
        warn_msg("Aborting handle.");
        handle->abort(result);
      }
    }
  }

  void info_msg(const std::string & msg) const
  {
    RCLCPP_INFO(
      node_logging_interface_->get_logger(),
      "[%s] [ActionServer] %s", action_name_.c_str(), msg.c_str());
  }

  void debug_msg(const std::string & msg) const
  {
    RCLCPP_DEBUG(
      node_logging_interface_->get_logger(),
      "[%s] [ActionServer] %s", action_name_.c_str(), msg.c_str());
  }

  void error_msg(const std::string & msg) const
  {
    RCLCPP_ERROR(
      node_logging_interface_->get_logger(),
      "[%s] [ActionServer] %s", action_name_.c_str(), msg.c_str());
  }

  void warn_msg(const std::string & msg) const
  {
    RCLCPP_WARN(
      node_logging_interface_->get_logger(),
      "[%s] [ActionServer] %s", action_name_.c_str(), msg.c_str());
  }
};

}  // namespace nav2_util

#endif   // NAV2_UTIL__SIMPLE_ACTION_SERVER_HPP_
