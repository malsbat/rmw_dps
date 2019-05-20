// Copyright 2018 Intel Corporation All rights reserved.
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

#ifndef RMW_DPS_CPP__CUSTOM_NODE_INFO_HPP_
#define RMW_DPS_CPP__CUSTOM_NODE_INFO_HPP_

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include <dps/dps.h>

#include "rmw/rmw.h"

#include "rmw_dps_cpp/namespace_prefix.hpp"

class NodeListener;

typedef struct CustomNodeInfo
{
  DPS_Node * node_;
  std::string uuid_;
  rmw_guard_condition_t * graph_guard_condition_;
  size_t domain_id_;
  std::vector<std::string> advertisement_topics_;
  DPS_Publication * advertisement_;
  DPS_Subscription * discover_;
  NodeListener * listener_;
} CustomNodeInfo;

inline bool
operator<(const DPS_UUID & lhs, const DPS_UUID & rhs)
{
  return DPS_UUIDCompare(&lhs, &rhs) < 0;
}

class NodeListener
{
public:
  struct Topic
  {
    std::string topic;
    std::vector<std::string> types;
    bool operator==(const Topic & that) const
    {
      return this->topic == that.topic &&
        this->types == that.types;
    }
  };
  struct Node
  {
    Node() : namespace_("/") {}
    std::string name;
    std::string namespace_;
    std::vector<Topic> publishers;
    std::vector<Topic> subscribers;
    bool operator==(const Node & that) const
    {
      return this->name == that.name &&
        this->namespace_ == that.namespace_ &&
        this->publishers == that.publishers &&
        this->subscribers == that.subscribers;
    }
  };

  explicit NodeListener(rmw_guard_condition_t * graph_guard_condition)
  : graph_guard_condition_(graph_guard_condition)
  {}

  static void
  onPublication(DPS_Subscription * sub, const DPS_Publication * pub, uint8_t * payload, size_t len)
  {
    RCUTILS_LOG_DEBUG_NAMED(
      "rmw_dps_cpp",
      "%s(sub=%p,pub=%p,payload=%p,len=%zu)", __FUNCTION__, (void*)sub, (void*)pub, payload, len);

    NodeListener * listener = reinterpret_cast<NodeListener *>(DPS_GetSubscriptionData(sub));
    std::lock_guard<std::mutex> lock(listener->mutex_);
    std::string uuid;
    Node node;
    for (size_t i = 0; i < DPS_PublicationGetNumTopics(pub); ++i) {
      std::string topic = DPS_PublicationGetTopic(pub, i);
      size_t pos;
      pos = topic.find(dps_uuid_prefix);
      if (pos != std::string::npos) {
        uuid = topic.substr(pos + strlen(dps_uuid_prefix));
        continue;
      }
      pos = topic.find(dps_namespace_prefix);
      if (pos != std::string::npos) {
        node.namespace_ = topic.substr(pos + strlen(dps_namespace_prefix));
        continue;
      }
      pos = topic.find(dps_name_prefix);
      if (pos != std::string::npos) {
        node.name = topic.substr(pos + strlen(dps_name_prefix));
        continue;
      }
      Topic publisher;
      if (process_topic_info(topic, dps_publisher_prefix, publisher)) {
        node.publishers.push_back(publisher);
        continue;
      }
      Topic subscriber;
      if (process_topic_info(topic, dps_subscriber_prefix, subscriber)) {
        node.subscribers.push_back(subscriber);
        continue;
      }
    }

    if (!uuid.empty()) {
      Node old_node = listener->discovered_nodes_[uuid];
      listener->discovered_nodes_[uuid] = node;
      if (!(old_node == node)) {
        rmw_ret_t ret = rmw_trigger_guard_condition(listener->graph_guard_condition_);
        if (ret != RMW_RET_OK) {
          RCUTILS_LOG_ERROR_NAMED(
            "rmw_dps_cpp",
            "failed to trigger guard condition");
        }
      }
    }
  }

  std::vector<Node>
  get_discovered_nodes() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Node> nodes(discovered_nodes_.size());
    size_t i = 0;
    for (auto it : discovered_nodes_) {
      nodes[i++] = it.second;
    }
    return nodes;
  }

  size_t
  count_publishers(const char * topic_name) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto it : discovered_nodes_) {
      count += std::count_if(it.second.publishers.begin(), it.second.publishers.end(),
        [topic_name](const Topic & publisher) { return publisher.topic == topic_name; });
    }
    return count;
  }

  size_t
  count_subscribers(const char * topic_name) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto it : discovered_nodes_) {
      count += std::count_if(it.second.subscribers.begin(), it.second.subscribers.end(),
        [topic_name](const Topic & subscriber) { return subscriber.topic == topic_name; });
    }
    return count;
  }

private:
  static bool
  process_topic_info(const std::string & topic_str, const char * prefix, Topic & topic)
  {
    size_t pos = topic_str.find(prefix);
    if (pos != std::string::npos) {
      pos = pos + strlen(prefix);
      size_t end_pos = topic_str.find("&types=");
      if (end_pos != std::string::npos) {
        topic.topic = topic_str.substr(pos, end_pos - pos);
        pos = end_pos + strlen("&types=");
      } else {
        topic.topic = topic_str.substr(pos);
      }
      while (pos != std::string::npos) {
        end_pos = topic_str.find(",", pos);
        if (end_pos != std::string::npos) {
          topic.types.push_back(topic_str.substr(pos, end_pos - pos));
          pos = end_pos + 1;
        } else {
          topic.types.push_back(topic_str.substr(pos));
          pos = end_pos;
        }
      }
      return true;
    }
    return false;
  }

  mutable std::mutex mutex_;
  std::map<std::string, Node> discovered_nodes_;
  rmw_guard_condition_t * graph_guard_condition_;
};

#endif  // RMW_DPS_CPP__CUSTOM_NODE_INFO_HPP_
