// Copyright 2014 Open Source Robotics Foundation, Inc.
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

#ifndef RCLCPP_RCLCPP_NODE_IMPL_HPP_
#define RCLCPP_RCLCPP_NODE_IMPL_HPP_

#include <algorithm>
#include <memory>
#include <string>

#include <rmw/rmw.h>
#include <rosidl_generator_cpp/message_type_support.hpp>
#include <rosidl_generator_cpp/service_type_support.hpp>

#include <rclcpp/contexts/default_context.hpp>

#ifndef RCLCPP_RCLCPP_NODE_HPP_
#include "node.hpp"
#endif

using namespace rclcpp;
using namespace rclcpp::node;

using rclcpp::contexts::default_context::DefaultContext;

Node::Node(std::string node_name)
: Node(node_name, DefaultContext::make_shared())
{}

Node::Node(std::string node_name, context::Context::SharedPtr context)
: name_(node_name), context_(context),
  number_of_subscriptions_(0), number_of_timers_(0), number_of_services_(0)
{
  node_handle_ = rmw_create_node(name_.c_str());
  using rclcpp::callback_group::CallbackGroupType;
  default_callback_group_ = \
    create_callback_group(CallbackGroupType::MutuallyExclusive);
}

rclcpp::callback_group::CallbackGroup::SharedPtr
Node::create_callback_group(
  rclcpp::callback_group::CallbackGroupType group_type)
{
  using rclcpp::callback_group::CallbackGroup;
  using rclcpp::callback_group::CallbackGroupType;
  auto group = CallbackGroup::SharedPtr(new CallbackGroup(group_type));
  callback_groups_.push_back(group);
  return group;
}

template<typename MessageT>
publisher::Publisher::SharedPtr
Node::create_publisher(std::string topic_name, size_t queue_size)
{
  using rosidl_generator_cpp::get_message_type_support_handle;
  auto type_support_handle = get_message_type_support_handle<MessageT>();
  rmw_publisher_t * publisher_handle = rmw_create_publisher(
    node_handle_, type_support_handle, topic_name.c_str(), queue_size);

  return publisher::Publisher::make_shared(publisher_handle);
}

bool
Node::group_in_node(callback_group::CallbackGroup::SharedPtr & group)
{
  bool group_belongs_to_this_node = false;
  for (auto & weak_group : this->callback_groups_) {
    auto cur_group = weak_group.lock();
    if (cur_group && (cur_group == group)) {
      group_belongs_to_this_node = true;
    }
  }
  return group_belongs_to_this_node;
}

template<typename MessageT>
typename subscription::Subscription<MessageT>::SharedPtr
Node::create_subscription(
  std::string topic_name,
  size_t queue_size,
  std::function<void(const std::shared_ptr<MessageT> &)> callback,
  rclcpp::callback_group::CallbackGroup::SharedPtr group)
{
  using rosidl_generator_cpp::get_message_type_support_handle;
  auto type_support_handle = get_message_type_support_handle<MessageT>();
  rmw_subscription_t * subscriber_handle = rmw_create_subscription(
    node_handle_, type_support_handle, topic_name.c_str(), queue_size);

  using namespace rclcpp::subscription;

  auto sub = Subscription<MessageT>::make_shared(
    subscriber_handle,
    topic_name,
    callback);
  auto sub_base_ptr = std::dynamic_pointer_cast<SubscriptionBase>(sub);
  if (group) {
    if (!group_in_node(group)) {
      // TODO: use custom exception
      throw std::runtime_error("Cannot create timer, group not in node.");
    }
    group->add_subscription(sub_base_ptr);
  } else {
    default_callback_group_->add_subscription(sub_base_ptr);
  }
  number_of_subscriptions_++;
  return sub;
}

rclcpp::timer::WallTimer::SharedPtr
Node::create_wall_timer(
  std::chrono::nanoseconds period,
  rclcpp::timer::CallbackType callback,
  rclcpp::callback_group::CallbackGroup::SharedPtr group)
{
  auto timer = rclcpp::timer::WallTimer::make_shared(period, callback);
  if (group) {
    if (!group_in_node(group)) {
      // TODO: use custom exception
      throw std::runtime_error("Cannot create timer, group not in node.");
    }
    group->add_timer(timer);
  } else {
    default_callback_group_->add_timer(timer);
  }
  number_of_timers_++;
  return timer;
}

rclcpp::timer::WallTimer::SharedPtr
Node::create_wall_timer(
  std::chrono::duration<long double, std::nano> period,
  rclcpp::timer::CallbackType callback,
  rclcpp::callback_group::CallbackGroup::SharedPtr group)
{
  return create_wall_timer(
    std::chrono::duration_cast<std::chrono::nanoseconds>(period),
    callback,
    group);
}

template<typename ServiceT>
typename client::Client<ServiceT>::SharedPtr
Node::create_client(
  const std::string & service_name,
  rclcpp::callback_group::CallbackGroup::SharedPtr group)
{
  using rosidl_generator_cpp::get_service_type_support_handle;
  auto service_type_support_handle =
    get_service_type_support_handle<ServiceT>();

  rmw_client_t * client_handle = rmw_create_client(
    this->node_handle_, service_type_support_handle, service_name.c_str());

  using namespace rclcpp::client;

  auto cli = Client<ServiceT>::make_shared(
    client_handle,
    service_name);

  auto cli_base_ptr = std::dynamic_pointer_cast<ClientBase>(cli);
  if (group) {
    if (!group_in_node(group)) {
      // TODO(esteve): use custom exception
      throw std::runtime_error("Cannot create client, group not in node.");
    }
    group->add_client(cli_base_ptr);
  } else {
    default_callback_group_->add_client(cli_base_ptr);
  }
  number_of_clients_++;

  return cli;
}

template<typename ServiceT, typename FunctorT>
typename rclcpp::service::Service<ServiceT>::SharedPtr
Node::create_service(
  const std::string & service_name,
  FunctorT callback,
  rclcpp::callback_group::CallbackGroup::SharedPtr group)
{
  using rosidl_generator_cpp::get_service_type_support_handle;
  auto service_type_support_handle =
    get_service_type_support_handle<ServiceT>();

  rmw_service_t * service_handle = rmw_create_service(
    this->node_handle_, service_type_support_handle, service_name.c_str());

  auto serv = create_service_internal<ServiceT>(
    service_handle, service_name, callback);
  auto serv_base_ptr = std::dynamic_pointer_cast<service::ServiceBase>(serv);
  if (group) {
    if (!group_in_node(group)) {
      // TODO: use custom exception
      throw std::runtime_error("Cannot create service, group not in node.");
    }
    group->add_service(serv_base_ptr);
  } else {
    default_callback_group_->add_service(serv_base_ptr);
  }
  number_of_services_++;
  return serv;
}

const std::vector<rcl_interfaces::SetParametersResult>
Node::set_parameters(
  const std::vector<rcl_interfaces::Parameter> & parameters)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<rcl_interfaces::SetParametersResult> results;
  for (auto p : parameters) {
    parameters_[p.name] = rclcpp::parameter::ParameterVariant::from_parameter(p);
    rcl_interfaces::SetParametersResult result;
    result.successful = true;
    // TODO: handle parameter constraints
    results.push_back(result);
  }
  return results;
}

const rcl_interfaces::SetParametersResult
Node::set_parameters_atomically(
  const std::vector<rcl_interfaces::Parameter> & parameters)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<std::string, rclcpp::parameter::ParameterVariant> tmp_map;
  for (auto p : parameters) {
    tmp_map[p.name] = rclcpp::parameter::ParameterVariant::from_parameter(p);
  }
  tmp_map.insert(parameters_.begin(), parameters_.end());
  std::swap(tmp_map, parameters_);
  // TODO: handle parameter constraints
  rcl_interfaces::SetParametersResult result;
  result.successful = true;
  return result;
}

const std::vector<rclcpp::parameter::ParameterVariant>
Node::get_parameters(
  const std::vector<std::string> & names)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<rclcpp::parameter::ParameterVariant> results;
  for (auto & kv : parameters_) {
    if (std::any_of(names.cbegin(), names.cend(), [&kv](const std::string & name) {
      return name == kv.first;
    }))
    {
      results.push_back(kv.second);
    }
  }
  return results;
}

const std::vector<rcl_interfaces::ParameterDescriptor>
Node::describe_parameters(
  const std::vector<std::string> & names)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<rcl_interfaces::ParameterDescriptor> results;
  for (auto & kv : parameters_) {
    if (std::any_of(names.cbegin(), names.cend(), [&kv](const std::string & name) {
      return name == kv.first;
    }))
    {
      rcl_interfaces::ParameterDescriptor parameter_descriptor;
      parameter_descriptor.name = kv.first;
      parameter_descriptor.parameter_type = kv.second.get_type();
      results.push_back(parameter_descriptor);
    }
  }
  return results;
}

const std::vector<uint8_t>
Node::get_parameter_types(
  const std::vector<std::string> & names)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<uint8_t> results;
  for (auto & kv : parameters_) {
    if (std::any_of(names.cbegin(), names.cend(), [&kv](const std::string & name) {
      return name == kv.first;
    }))
    {
      results.push_back(kv.second.get_type());
    } else {
      results.push_back(rcl_interfaces::ParameterType::PARAMETER_NOT_SET);
    }
  }
  return results;
}

const std::vector<rcl_interfaces::ListParametersResult>
Node::list_parameters(
  const std::vector<std::string> & prefixes, uint64_t depth)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<rcl_interfaces::ListParametersResult> results;

  // TODO: define parameter separator, use "." for now
  for (auto & kv : parameters_) {
    if (std::any_of(prefixes.cbegin(), prefixes.cend(), [&kv, &depth](const std::string & prefix) {
      if (kv.first.find(prefix + ".") == 0) {
        size_t length = prefix.length();
        std::string substr = kv.first.substr(length);
        return std::count(substr.begin(), substr.end(), '.') < depth;
      }
      return false;
    }))
    {
      rcl_interfaces::ListParametersResult result;
      result.parameter_names.push_back(kv.first);
      size_t last_separator = kv.first.find_last_of('.');
      std::string prefix = kv.first.substr(0, last_separator);
      if (std::find(result.parameter_prefixes.cbegin(), result.parameter_prefixes.cend(),
        prefix) == result.parameter_prefixes.cend())
      {
        result.parameter_prefixes.push_back(prefix);
      }
      results.push_back(result);
    }
  }
  return results;
}
#endif /* RCLCPP_RCLCPP_NODE_IMPL_HPP_ */
