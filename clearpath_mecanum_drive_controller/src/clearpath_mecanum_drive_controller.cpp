// Copyright (c) 2023, Stogl Robotics Consulting UG (haftungsbeschränkt)
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

#include "clearpath_mecanum_drive_controller/clearpath_mecanum_drive_controller.hpp"

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "controller_interface/helpers.hpp"
#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "lifecycle_msgs/msg/state.hpp"
#include "tf2/transform_datatypes.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.hpp"

namespace
{  // utility

using ControllerReferenceMsg =
  clearpath_mecanum_drive_controller::MecanumDriveController::ControllerReferenceMsg;

using ControllerReferenceUnstampedMsg =
  clearpath_mecanum_drive_controller::MecanumDriveController::ControllerReferenceUnstampedMsg;

// called from RT control loop
void reset_controller_reference_unstamped_msg(
  const std::shared_ptr<ControllerReferenceUnstampedMsg> & msg)
{
  msg->linear.x = std::numeric_limits<double>::quiet_NaN();
  msg->linear.y = std::numeric_limits<double>::quiet_NaN();
  msg->linear.z = std::numeric_limits<double>::quiet_NaN();
  msg->angular.x = std::numeric_limits<double>::quiet_NaN();
  msg->angular.y = std::numeric_limits<double>::quiet_NaN();
  msg->angular.z = std::numeric_limits<double>::quiet_NaN();
}

void reset_controller_reference_msg(
  const std::shared_ptr<ControllerReferenceMsg> & msg,
  const std::shared_ptr<rclcpp_lifecycle::LifecycleNode> & node)
{
  msg->header.stamp = node->now();
  msg->twist.linear.x = std::numeric_limits<double>::quiet_NaN();
  msg->twist.linear.y = std::numeric_limits<double>::quiet_NaN();
  msg->twist.linear.z = std::numeric_limits<double>::quiet_NaN();
  msg->twist.angular.x = std::numeric_limits<double>::quiet_NaN();
  msg->twist.angular.y = std::numeric_limits<double>::quiet_NaN();
  msg->twist.angular.z = std::numeric_limits<double>::quiet_NaN();
}


}  // namespace

namespace clearpath_mecanum_drive_controller
{
MecanumDriveController::MecanumDriveController()
: controller_interface::ChainableControllerInterface()
{
}

controller_interface::CallbackReturn MecanumDriveController::on_init()
{
  try
  {
    param_listener_ = std::make_shared<clearpath_mecanum_drive_controller::ParamListener>(get_node());
  }
  catch (const std::exception & e)
  {
    fprintf(stderr, "Exception thrown during controller's init with message: %s \n", e.what());
    return controller_interface::CallbackReturn::ERROR;
  }

  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn MecanumDriveController::on_configure(
  const rclcpp_lifecycle::State & /*previous_state*/)
{
  params_ = param_listener_->get_params();

  if (!params_.state_joint_names.empty())
  {
    state_joint_names_ = params_.state_joint_names;
  }
  else
  {
    state_joint_names_ = params_.command_joint_names;
  }

  if (params_.command_joint_names.size() != state_joint_names_.size())
  {
    RCLCPP_FATAL(
      get_node()->get_logger(),
      "Size of 'joints' (%ld) and 'state_joint_names' (%ld) parameters has "
      "to be the same!",
      params_.command_joint_names.size(), state_joint_names_.size());
    return CallbackReturn::FAILURE;
  }

  // Initialize odometry
  odometry_.init(get_node()->get_clock()->now(), {params_.kinematics.base_frame_offset.x, params_.kinematics.base_frame_offset.y, params_.kinematics.base_frame_offset.theta});

  // Set wheel params for the odometry computation
  odometry_.setWheelsParams(
    params_.kinematics.sum_of_robot_center_projection_on_X_Y_axis,
    params_.kinematics.wheels_radius);

  // topics QoS
  auto subscribers_qos = rclcpp::SystemDefaultsQoS();
  subscribers_qos.keep_last(1);
  subscribers_qos.best_effort();

  // Reference Subscriber
  use_stamped_vel_ = params_.use_stamped_vel;
  ref_timeout_ = rclcpp::Duration::from_seconds(params_.reference_timeout);
  cmd_timeout_ = rclcpp::Duration::from_seconds(params_.command_timeout);
  cmd_timestamp_ = get_node()->now();
  if (use_stamped_vel_)
  {
    ref_subscriber_ = get_node()->create_subscription<ControllerReferenceMsg>(
    "~/cmd_vel", subscribers_qos,
    std::bind(&MecanumDriveController::reference_callback, this, std::placeholders::_1));

    std::shared_ptr<ControllerReferenceMsg> msg = std::make_shared<ControllerReferenceMsg>();
    reset_controller_reference_msg(msg, get_node());
    input_ref_.writeFromNonRT(msg);
  }
  else
  {
    ref_unstamped_subscriber_ = get_node()->create_subscription<ControllerReferenceUnstampedMsg>(
      "~/cmd_vel_unstamped", subscribers_qos,
      std::bind(&MecanumDriveController::reference_unstamped_callback, this, std::placeholders::_1));
    std::shared_ptr<ControllerReferenceUnstampedMsg> msg = std::make_shared<ControllerReferenceUnstampedMsg>();
    reset_controller_reference_unstamped_msg(msg);
    input_ref_unstamped_.writeFromNonRT(msg);
  }

  try
  {
    // Odom state publisher
    odom_s_publisher_ =
      get_node()->create_publisher<OdomStateMsg>("~/odom", rclcpp::SystemDefaultsQoS());
    rt_odom_state_publisher_ = std::make_unique<OdomStatePublisher>(odom_s_publisher_);
  }
  catch (const std::exception & e)
  {
    fprintf(
      stderr, "Exception thrown during publisher creation at configure stage with message : %s \n",
      e.what());
    return controller_interface::CallbackReturn::ERROR;
  }

  rt_odom_state_publisher_->lock();
  rt_odom_state_publisher_->msg_.header.stamp = get_node()->now();
  rt_odom_state_publisher_->msg_.header.frame_id = params_.odom_frame_id;
  rt_odom_state_publisher_->msg_.child_frame_id = params_.base_frame_id;
  rt_odom_state_publisher_->msg_.pose.pose.position.z = 0;

  auto & pose_covariance = rt_odom_state_publisher_->msg_.pose.covariance;
  auto & twist_covariance = rt_odom_state_publisher_->msg_.twist.covariance;
  constexpr size_t NUM_DIMENSIONS = 6;
  for (size_t index = 0; index < 6; ++index)
  {
    const size_t diagonal_index = NUM_DIMENSIONS * index + index;
    pose_covariance[diagonal_index] = params_.pose_covariance_diagonal[index];
    twist_covariance[diagonal_index] = params_.twist_covariance_diagonal[index];
  }
  rt_odom_state_publisher_->unlock();

  try
  {
    // Tf State publisher
    tf_odom_s_publisher_ =
      get_node()->create_publisher<TfStateMsg>("~/tf_odometry", rclcpp::SystemDefaultsQoS());
    rt_tf_odom_state_publisher_ = std::make_unique<TfStatePublisher>(tf_odom_s_publisher_);
  }
  catch (const std::exception & e)
  {
    fprintf(
      stderr, "Exception thrown during publisher creation at configure stage with message : %s \n",
      e.what());
    return controller_interface::CallbackReturn::ERROR;
  }

  rt_tf_odom_state_publisher_->lock();
  rt_tf_odom_state_publisher_->msg_.transforms.resize(1);
  rt_tf_odom_state_publisher_->msg_.transforms[0].header.stamp = get_node()->now();
  rt_tf_odom_state_publisher_->msg_.transforms[0].header.frame_id = params_.odom_frame_id;
  rt_tf_odom_state_publisher_->msg_.transforms[0].child_frame_id = params_.base_frame_id;
  rt_tf_odom_state_publisher_->msg_.transforms[0].transform.translation.z = 0.0;
  rt_tf_odom_state_publisher_->unlock();

  try
  {
    // controller State publisher
    controller_s_publisher_ = get_node()->create_publisher<ControllerStateMsg>(
      "~/controller_state", rclcpp::SystemDefaultsQoS());
    controller_state_publisher_ =
      std::make_unique<ControllerStatePublisher>(controller_s_publisher_);
  }
  catch (const std::exception & e)
  {
    fprintf(
      stderr,
      "Exception thrown during publisher creation at configure stage "
      "with message : %s \n",
      e.what());
    return controller_interface::CallbackReturn::ERROR;
  }

  controller_state_publisher_->lock();
  controller_state_publisher_->msg_.header.stamp = get_node()->now();
  controller_state_publisher_->msg_.header.frame_id = params_.odom_frame_id;
  controller_state_publisher_->unlock();

  RCLCPP_INFO(get_node()->get_logger(), "configure successful");
  return controller_interface::CallbackReturn::SUCCESS;
}

void MecanumDriveController::reference_callback(const std::shared_ptr<ControllerReferenceMsg> msg)
{
  // if no timestamp provided use current time for command timestamp
  if (msg->header.stamp.sec == 0 && msg->header.stamp.nanosec == 0u)
  {
    RCLCPP_WARN(
      get_node()->get_logger(),
      "Timestamp in header is missing, using current time as command "
      "timestamp.");
    msg->header.stamp = get_node()->now();
  }
  const auto age_of_last_command = get_node()->now() - msg->header.stamp;

  if (ref_timeout_ == rclcpp::Duration::from_seconds(0) || age_of_last_command <= ref_timeout_)
  {
    input_ref_.writeFromNonRT(msg);
  }
  else
  {
    RCLCPP_ERROR(
      get_node()->get_logger(),
      "Received message has timestamp %.10f older for %.10f which is more then allowed timeout "
      "(%.4f).",
      rclcpp::Time(msg->header.stamp).seconds(), age_of_last_command.seconds(),
      ref_timeout_.seconds());
    reset_controller_reference_msg(msg, get_node());
  }
}

void MecanumDriveController::reference_unstamped_callback(const std::shared_ptr<ControllerReferenceUnstampedMsg> msg)
{
  cmd_timestamp_ = get_node()->now();
  input_ref_unstamped_.writeFromNonRT(msg);
}

controller_interface::InterfaceConfiguration
MecanumDriveController::command_interface_configuration() const
{
  controller_interface::InterfaceConfiguration command_interfaces_config;
  command_interfaces_config.type = controller_interface::interface_configuration_type::INDIVIDUAL;

  command_interfaces_config.names.reserve(params_.command_joint_names.size());
  for (const auto & joint : params_.command_joint_names)
  {
    command_interfaces_config.names.push_back(joint + "/" + params_.interface_name);
  }

  return command_interfaces_config;
}

controller_interface::InterfaceConfiguration MecanumDriveController::state_interface_configuration()
  const
{
  controller_interface::InterfaceConfiguration state_interfaces_config;
  state_interfaces_config.type = controller_interface::interface_configuration_type::INDIVIDUAL;

  state_interfaces_config.names.reserve(state_joint_names_.size());

  for (const auto & joint : state_joint_names_)
  {
    state_interfaces_config.names.push_back(joint + "/" + params_.interface_name);
  }

  return state_interfaces_config;
}

std::vector<hardware_interface::CommandInterface>
MecanumDriveController::on_export_reference_interfaces()
{
  reference_interfaces_.resize(NR_REF_ITFS, std::numeric_limits<double>::quiet_NaN());

  std::vector<hardware_interface::CommandInterface> reference_interfaces;

  reference_interfaces.reserve(reference_interfaces_.size());

  std::vector<std::string> reference_interface_names = {
    "linear/x/velocity", "linear/y/velocity", "angular/z/velocity"};

  for (size_t i = 0; i < reference_interfaces_.size(); ++i)
  {
    reference_interfaces.push_back(hardware_interface::CommandInterface(
      get_node()->get_name(), reference_interface_names[i], &reference_interfaces_[i]));
  }

  return reference_interfaces;
}

bool MecanumDriveController::on_set_chained_mode(bool chained_mode)
{
  // Always accept switch to/from chained mode
  return true || chained_mode;
}

controller_interface::CallbackReturn MecanumDriveController::on_activate(
  const rclcpp_lifecycle::State & /*previous_state*/)
{
  // Set default value in command
  if(use_stamped_vel_){
    reset_controller_reference_msg(*(input_ref_.readFromRT()), get_node());
  }
  else
  {
    reset_controller_reference_unstamped_msg(*(input_ref_unstamped_.readFromRT()));
  }
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn MecanumDriveController::on_deactivate(
  const rclcpp_lifecycle::State & /*previous_state*/)
{
  for (size_t i = 0; i < NR_CMD_ITFS; ++i)
  {
    command_interfaces_[i].set_value(std::numeric_limits<double>::quiet_NaN());
  }
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::return_type MecanumDriveController::update_reference_from_subscribers()
{
  // Move functionality to the `update_and_write_commands` because of the missing arguments in
  // humble - otherwise issues with multiple time-sources might happen when working with simulators

  return controller_interface::return_type::OK;
}

controller_interface::return_type MecanumDriveController::update_and_write_commands(
  const rclcpp::Time & time, const rclcpp::Duration & period)
{
  if(use_stamped_vel_)
  {
    // Moved from update_reference_from_subscribers
    auto current_ref = *(input_ref_.readFromRT());
    const auto age_of_last_command = time - (current_ref)->header.stamp;
    // send message only if there is no timeout
    if (age_of_last_command <= ref_timeout_ || ref_timeout_ == rclcpp::Duration::from_seconds(0))
    {
      if (
        !std::isnan(current_ref->twist.linear.x) && !std::isnan(current_ref->twist.linear.y) &&
        !std::isnan(current_ref->twist.angular.z))
      {
        reference_interfaces_[0] = current_ref->twist.linear.x;
        reference_interfaces_[1] = current_ref->twist.linear.y;
        reference_interfaces_[2] = current_ref->twist.angular.z;

        if (ref_timeout_ == rclcpp::Duration::from_seconds(0))
        {
          current_ref->twist.linear.x = std::numeric_limits<double>::quiet_NaN();
          current_ref->twist.linear.y = std::numeric_limits<double>::quiet_NaN();
          current_ref->twist.angular.z = std::numeric_limits<double>::quiet_NaN();
        }
      }
    }
    else
    {
      if (
        !std::isnan(current_ref->twist.linear.x) && !std::isnan(current_ref->twist.linear.y) &&
        !std::isnan(current_ref->twist.angular.z))
      {
        reference_interfaces_[0] = 0.0;
        reference_interfaces_[1] = 0.0;
        reference_interfaces_[2] = 0.0;

        current_ref->twist.linear.x = std::numeric_limits<double>::quiet_NaN();
        current_ref->twist.linear.y = std::numeric_limits<double>::quiet_NaN();
        current_ref->twist.angular.z = std::numeric_limits<double>::quiet_NaN();
      }
    }
  }
  else
  {
    auto current_ref_unstamped = *(input_ref_unstamped_.readFromRT());
    if (
        !std::isnan(current_ref_unstamped->linear.x) && !std::isnan(current_ref_unstamped->linear.y) &&
        !std::isnan(current_ref_unstamped->angular.z))
    {
      reference_interfaces_[0] = current_ref_unstamped->linear.x;
      reference_interfaces_[1] = current_ref_unstamped->linear.y;
      reference_interfaces_[2] = current_ref_unstamped->angular.z;

      current_ref_unstamped->linear.x = std::numeric_limits<double>::quiet_NaN();
      current_ref_unstamped->linear.y = std::numeric_limits<double>::quiet_NaN();
      current_ref_unstamped->angular.z = std::numeric_limits<double>::quiet_NaN();
    }
  }

  // FORWARD KINEMATICS (odometry).
  double wheel_front_left_vel = state_interfaces_[0].get_value();
  double wheel_back_left_vel = state_interfaces_[1].get_value();
  double wheel_back_right_vel = state_interfaces_[2].get_value();
  double wheel_front_right_vel = state_interfaces_[3].get_value();

  if (
    !std::isnan(wheel_front_left_vel) && !std::isnan(wheel_back_left_vel) &&
    !std::isnan(wheel_back_right_vel) && !std::isnan(wheel_front_right_vel))
  {
    // Estimate twist (using joint information) and integrate
    odometry_.update(
      wheel_front_left_vel, wheel_back_left_vel, wheel_back_right_vel, wheel_front_right_vel,
      period.seconds());
  }

  // INVERSE KINEMATICS (move robot).
  // Compute wheels velocities (this is the actual ik):
  // NOTE: the input desired twist (from topic `~/reference`) is a body twist.
  if (
    !std::isnan(reference_interfaces_[0]) && !std::isnan(reference_interfaces_[1]) &&
    !std::isnan(reference_interfaces_[2]))
  {
    tf2::Quaternion quaternion;
    quaternion.setRPY(0.0, 0.0, params_.kinematics.base_frame_offset.theta);
    /// \note The variables meaning:
    /// rotation_from_base_to_center: Rotation transformation matrix, to transform from
    /// base frame to center frame
    /// linear_trans_from_base_to_center: offset/linear transformation matrix, to
    /// transform from base frame to center frame

    tf2::Matrix3x3 rotation_from_base_to_center = tf2::Matrix3x3((quaternion));
    tf2::Vector3 velocity_in_base_frame_w_r_t_center_frame_ =
      rotation_from_base_to_center *
      tf2::Vector3(reference_interfaces_[0], reference_interfaces_[1], 0.0);
    tf2::Vector3 linear_trans_from_base_to_center = tf2::Vector3(
      params_.kinematics.base_frame_offset.x, params_.kinematics.base_frame_offset.y, 0.0);

    velocity_in_center_frame_linear_x_ =
      velocity_in_base_frame_w_r_t_center_frame_.x() +
      linear_trans_from_base_to_center.y() * reference_interfaces_[2];
    velocity_in_center_frame_linear_y_ =
      velocity_in_base_frame_w_r_t_center_frame_.y() -
      linear_trans_from_base_to_center.x() * reference_interfaces_[2];
    velocity_in_center_frame_angular_z_ = reference_interfaces_[2];

    double w_front_left_vel =
      1.0 / params_.kinematics.wheels_radius *
      (velocity_in_center_frame_linear_x_ - velocity_in_center_frame_linear_y_ -
       params_.kinematics.sum_of_robot_center_projection_on_X_Y_axis *
         velocity_in_center_frame_angular_z_);
    double w_back_left_vel =
      1.0 / params_.kinematics.wheels_radius *
      (velocity_in_center_frame_linear_x_ + velocity_in_center_frame_linear_y_ -
       params_.kinematics.sum_of_robot_center_projection_on_X_Y_axis *
         velocity_in_center_frame_angular_z_);
    double w_back_right_vel =
      1.0 / params_.kinematics.wheels_radius *
      (velocity_in_center_frame_linear_x_ - velocity_in_center_frame_linear_y_ +
       params_.kinematics.sum_of_robot_center_projection_on_X_Y_axis *
         velocity_in_center_frame_angular_z_);
    double w_front_right_vel =
      1.0 / params_.kinematics.wheels_radius *
      (velocity_in_center_frame_linear_x_ + velocity_in_center_frame_linear_y_ +
       params_.kinematics.sum_of_robot_center_projection_on_X_Y_axis *
         velocity_in_center_frame_angular_z_);

    // Set wheels velocities:
    command_interfaces_[0].set_value(w_front_left_vel);
    command_interfaces_[1].set_value(w_back_left_vel);
    command_interfaces_[2].set_value(w_back_right_vel);
    command_interfaces_[3].set_value(w_front_right_vel);
  }
  else
  {
    if(cmd_timeout_ < get_node()->now() - cmd_timestamp_)
    {
      command_interfaces_[0].set_value(0.0);
      command_interfaces_[1].set_value(0.0);
      command_interfaces_[2].set_value(0.0);
      command_interfaces_[3].set_value(0.0);
    }
  }

  // Publish odometry message
  // Compute and store orientation info
  tf2::Quaternion orientation;
  orientation.setRPY(0.0, 0.0, odometry_.getRz());

  // Populate odom message and publish
  if (rt_odom_state_publisher_->trylock())
  {
    rt_odom_state_publisher_->msg_.header.stamp = time;
    rt_odom_state_publisher_->msg_.pose.pose.position.x = odometry_.getX();
    rt_odom_state_publisher_->msg_.pose.pose.position.y = odometry_.getY();
    rt_odom_state_publisher_->msg_.pose.pose.orientation = tf2::toMsg(orientation);
    rt_odom_state_publisher_->msg_.twist.twist.linear.x = odometry_.getVx();
    rt_odom_state_publisher_->msg_.twist.twist.linear.y = odometry_.getVy();
    rt_odom_state_publisher_->msg_.twist.twist.angular.z = odometry_.getWz();
    rt_odom_state_publisher_->unlockAndPublish();
  }

  // Publish tf /odom frame
  if (params_.enable_odom_tf && rt_tf_odom_state_publisher_->trylock())
  {
    rt_tf_odom_state_publisher_->msg_.transforms.front().header.stamp = time;
    rt_tf_odom_state_publisher_->msg_.transforms.front().transform.translation.x = odometry_.getX();
    rt_tf_odom_state_publisher_->msg_.transforms.front().transform.translation.y = odometry_.getY();
    rt_tf_odom_state_publisher_->msg_.transforms.front().transform.rotation =
      tf2::toMsg(orientation);
    rt_tf_odom_state_publisher_->unlockAndPublish();
  }

  if (controller_state_publisher_->trylock())
  {
    controller_state_publisher_->msg_.header.stamp = get_node()->now();
    controller_state_publisher_->msg_.front_left_wheel_velocity = state_interfaces_[0].get_value();
    controller_state_publisher_->msg_.back_left_wheel_velocity = state_interfaces_[1].get_value();
    controller_state_publisher_->msg_.back_right_wheel_velocity = state_interfaces_[2].get_value();
    controller_state_publisher_->msg_.front_right_wheel_velocity = state_interfaces_[3].get_value();
  //  controller_state_publisher_->msg_.front_left_wheel_velocity = command_interfaces_[0].get_value();
  //  controller_state_publisher_->msg_.back_left_wheel_velocity = command_interfaces_[1].get_value();
  //  controller_state_publisher_->msg_.back_right_wheel_velocity = command_interfaces_[2].get_value();
  //  controller_state_publisher_->msg_.front_right_wheel_velocity = command_interfaces_[3].get_value();
    controller_state_publisher_->msg_.reference_velocity.linear.x = reference_interfaces_[0];
    controller_state_publisher_->msg_.reference_velocity.linear.y = reference_interfaces_[1];
    controller_state_publisher_->msg_.reference_velocity.angular.z = reference_interfaces_[2];
    controller_state_publisher_->unlockAndPublish();
  }

  reference_interfaces_[0] = std::numeric_limits<double>::quiet_NaN();
  reference_interfaces_[1] = std::numeric_limits<double>::quiet_NaN();
  reference_interfaces_[2] = std::numeric_limits<double>::quiet_NaN();

  return controller_interface::return_type::OK;
}

}  // namespace clearpath_mecanum_drive_controller

#include "pluginlib/class_list_macros.hpp"

PLUGINLIB_EXPORT_CLASS(
  clearpath_mecanum_drive_controller::MecanumDriveController,
  controller_interface::ChainableControllerInterface)
