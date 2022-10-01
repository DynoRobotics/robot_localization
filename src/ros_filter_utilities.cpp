/*
 * Copyright (c) 2014, 2015, 2016 Charles River Analytics, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 * with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "robot_localization/ros_filter_utilities.hpp"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "Eigen/Dense"
#include "geometry_msgs/msg/transform_stamped.hpp"
#include "rclcpp/time.hpp"
#include "robot_localization/filter_common.hpp"
#include "robot_localization/filter_utilities.hpp"
#include "tf2/LinearMath/Matrix3x3.h"
#include "tf2/LinearMath/Quaternion.h"
#include "tf2/LinearMath/Transform.h"
#include "tf2/LinearMath/Vector3.h"
#include "tf2/time.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.hpp"
#include "tf2_ros/buffer.h"

#define THROTTLE(clock, duration, thing) do { \
    static rclcpp::Time _last_output_time ## __LINE__(0, 0, (clock)->get_clock_type()); \
    auto _now = (clock)->now(); \
    if (_now - _last_output_time ## __LINE__ > (duration)) { \
      _last_output_time ## __LINE__ = _now; \
      thing; \
    } \
} while (0)

std::ostream & operator<<(std::ostream & os, const tf2::Vector3 & vec)
{
  os << "(" << std::setprecision(20) << vec.getX() << " " << vec.getY() << " " <<
    vec.getZ() << ")\n";

  return os;
}

std::ostream & operator<<(std::ostream & os, const tf2::Quaternion & quat)
{
  double roll, pitch, yaw;
  tf2::Matrix3x3 or_tmp(quat);
  or_tmp.getRPY(roll, pitch, yaw);

  os << "(" << std::setprecision(20) << roll << ", " << pitch << ", " << yaw <<
    ")\n";

  return os;
}

std::ostream & operator<<(std::ostream & os, const tf2::Transform & trans)
{
  os << "Origin: " << trans.getOrigin() <<
    "Rotation (RPY): " << trans.getRotation();

  return os;
}

std::ostream & operator<<(std::ostream & os, const std::vector<double> & vec)
{
  os << "(" << std::setprecision(20);

  for (size_t i = 0; i < vec.size(); ++i) {
    os << vec[i] << " ";
  }

  os << ")\n";

  return os;
}

std::ostream & operator<<(std::ostream & os, const std::vector<bool> & vec)
{
  os << "(" << std::boolalpha;

  for (size_t i = 0; i < vec.size(); ++i) {
    os << vec[i] << " ";
  }

  os << ")\n";

  return os;
}

namespace robot_localization
{
namespace ros_filter_utilities
{

double getYaw(const tf2::Quaternion quat)
{
  tf2::Matrix3x3 mat(quat);

  double dummy;
  double yaw;
  mat.getRPY(dummy, dummy, yaw);

  return yaw;
}

bool lookupTransformSafe(
  rclcpp::Node * node,
  const tf2_ros::Buffer * buffer,
  const std::string & target_frame,
  const std::string & source_frame,
  const rclcpp::Time & time,
  const rclcpp::Duration & duration,
  tf2::Transform & target_frame_trans,
  const bool silent)
{
  // Transforming from a frame id to itself can fail when the tf tree isn't
  // being broadcast (e.g., for some bag files). We don't need to transform
  // anything so might as well return identity and true.
  if (target_frame == source_frame) {
    target_frame_trans.setIdentity();
    return true;
  }

  tf2::TimePoint time_tf = tf2::timeFromSec(filter_utilities::toSec(time));
  tf2::Duration duration_tf = tf2::durationFromSec(filter_utilities::toSec(duration));

  std::string error_msg;

  // First try to transform the data at the requested time
  if (buffer->canTransform(target_frame, source_frame, time_tf, duration_tf, &error_msg)) {
    try {
      geometry_msgs::msg::TransformStamped stamped = buffer->lookupTransform(
        target_frame, source_frame, time_tf, duration_tf);
      tf2::fromMsg(stamped.transform, target_frame_trans);
      return true;
    } catch (tf2::TransformException & ex) {
      // Try using the latest available transform instead, see further down.
    }
  }
  
  // The issue might be that the transforms that are available are not close
  // enough temporally to be used. In that case, just use the latest available
  // transform and warn the user.
  if (buffer->canTransform(target_frame, source_frame, tf2::TimePointZero, duration_tf, &error_msg)) {
    try {
      tf2::fromMsg(
        buffer
        ->lookupTransform(
          target_frame, source_frame,
          tf2::TimePointZero, duration_tf)
        .transform,
        target_frame_trans);

      if (!silent) {
        // ROS_WARN_STREAM_THROTTLE(2.0, "Transform from " << source_frame <<
        // " to " << target_frame << " was unavailable for the time requested. Using latest instead.\n");
      }

      return true;
    } catch (tf2::TransformException & ex) {
      if (!silent) {
        // ROS_WARN_STREAM_THROTTLE(2.0, "Could not obtain transform from " <<
        // source_frame << " to " << target_frame << ". Error was " << ex.what() << "\n");
      }
    }
  } else {
    if (!silent) {
      // ROS_WARN_STREAM_THROTTLE(2.0, "Could not obtain transform from " <<
      // source_frame << " to " << target_frame << "\n");
    }
  }

  if (!silent) {
    auto & clk = *node->get_clock();
    RCLCPP_WARN_STREAM_THROTTLE(node->get_logger(), clk, 3000, "Could not transform from " << source_frame << " to " << target_frame);
  }

  return false;
}

bool lookupTransformSafe(
  rclcpp::Node * node,
  const tf2_ros::Buffer * buffer,
  const std::string & target_frame,
  const std::string & source_frame,
  const rclcpp::Time & time,
  tf2::Transform & target_frame_trans,
  const bool silent)
{
  return lookupTransformSafe(
    node, buffer, target_frame, source_frame, time,
    rclcpp::Duration(0, 0u), target_frame_trans, silent);
}

void quatToRPY(
  const tf2::Quaternion & quat, double & roll, double & pitch,
  double & yaw)
{
  tf2::Matrix3x3 or_tmp(quat);
  or_tmp.getRPY(roll, pitch, yaw);
}

void stateToTF(const Eigen::VectorXd & state, tf2::Transform & state_tf)
{
  state_tf.setOrigin(
    tf2::Vector3(
      state(StateMemberX), state(StateMemberY),
      state(StateMemberZ)));
  tf2::Quaternion quat;
  quat.setRPY(
    state(StateMemberRoll), state(StateMemberPitch),
    state(StateMemberYaw));

  state_tf.setRotation(quat);
}

void TFtoState(const tf2::Transform & state_tf, Eigen::VectorXd & state)
{
  state(StateMemberX) = state_tf.getOrigin().getX();
  state(StateMemberY) = state_tf.getOrigin().getY();
  state(StateMemberZ) = state_tf.getOrigin().getZ();
  quatToRPY(
    state_tf.getRotation(), state(StateMemberRoll),
    state(StateMemberPitch), state(StateMemberYaw));
}

}  // namespace ros_filter_utilities
}  // namespace robot_localization
