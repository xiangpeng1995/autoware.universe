// Copyright 2023 TIER IV, Inc.
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

#include "behavior_path_planner/utils/path_safety_checker/objects_filtering.hpp"

#include "behavior_path_planner/utils/utils.hpp"

#include <motion_utils/trajectory/interpolation.hpp>
#include <tier4_autoware_utils/geometry/boost_polygon_utils.hpp>
#include <tier4_autoware_utils/geometry/path_with_lane_id_geometry.hpp>

namespace behavior_path_planner::utils::path_safety_checker
{

PredictedObjects filterObjects(
  const std::shared_ptr<const PredictedObjects> & objects,
  const std::shared_ptr<RouteHandler> & route_handler, const lanelet::ConstLanelets & current_lanes,
  const geometry_msgs::msg::Point & current_pose,
  const std::shared_ptr<ObjectsFilteringParams> & params)
{
  // Guard
  if (objects->objects.empty()) {
    return PredictedObjects();
  }

  const double ignore_object_velocity_threshold = params->ignore_object_velocity_threshold;
  const double object_check_forward_distance = params->object_check_forward_distance;
  const double object_check_backward_distance = params->object_check_backward_distance;
  const ObjectTypesToCheck & target_object_types = params->object_types_to_check;

  PredictedObjects filtered_objects;

  filtered_objects = filterObjectsByVelocity(*objects, ignore_object_velocity_threshold, false);

  filterObjectsByClass(filtered_objects, target_object_types);

  const auto path = route_handler->getCenterLinePath(
    current_lanes, object_check_backward_distance, object_check_forward_distance);

  filterObjectsByPosition(
    filtered_objects, path.points, current_pose, object_check_forward_distance,
    object_check_backward_distance);

  return filtered_objects;
}

PredictedObjects filterObjectsByVelocity(
  const PredictedObjects & objects, const double velocity_threshold,
  const bool remove_above_threshold)
{
  if (remove_above_threshold) {
    return filterObjectsByVelocity(objects, -velocity_threshold, velocity_threshold);
  } else {
    return filterObjectsByVelocity(objects, velocity_threshold, std::numeric_limits<double>::max());
  }
}

PredictedObjects filterObjectsByVelocity(
  const PredictedObjects & objects, double velocity_threshold, double max_velocity)
{
  PredictedObjects filtered;
  filtered.header = objects.header;
  for (const auto & obj : objects.objects) {
    const auto v_norm = std::hypot(
      obj.kinematics.initial_twist_with_covariance.twist.linear.x,
      obj.kinematics.initial_twist_with_covariance.twist.linear.y);
    if (velocity_threshold < v_norm && v_norm < max_velocity) {
      filtered.objects.push_back(obj);
    }
  }
  return filtered;
}

void filterObjectsByPosition(
  PredictedObjects & objects, const std::vector<PathPointWithLaneId> & path_points,
  const geometry_msgs::msg::Point & current_pose, const double forward_distance,
  const double backward_distance)
{
  // Create a new container to hold the filtered objects
  PredictedObjects filtered;
  filtered.header = objects.header;

  // Reserve space in the vector to avoid reallocations
  filtered.objects.reserve(objects.objects.size());

  for (const auto & obj : objects.objects) {
    const double dist_ego_to_obj = motion_utils::calcSignedArcLength(
      path_points, current_pose, obj.kinematics.initial_pose_with_covariance.pose.position);

    if (-backward_distance < dist_ego_to_obj && dist_ego_to_obj < forward_distance) {
      filtered.objects.push_back(obj);
    }
  }

  // Replace the original objects with the filtered list
  objects.objects = std::move(filtered.objects);
  return;
}

void filterObjectsByClass(
  PredictedObjects & objects, const ObjectTypesToCheck & target_object_types)
{
  using autoware_auto_perception_msgs::msg::ObjectClassification;

  PredictedObjects filtered_objects;

  for (auto & object : objects.objects) {
    const auto t = utils::getHighestProbLabel(object.classification);
    const auto is_object_type =
      ((t == ObjectClassification::CAR && target_object_types.check_car) ||
       (t == ObjectClassification::TRUCK && target_object_types.check_truck) ||
       (t == ObjectClassification::BUS && target_object_types.check_bus) ||
       (t == ObjectClassification::TRAILER && target_object_types.check_trailer) ||
       (t == ObjectClassification::UNKNOWN && target_object_types.check_unknown) ||
       (t == ObjectClassification::BICYCLE && target_object_types.check_bicycle) ||
       (t == ObjectClassification::MOTORCYCLE && target_object_types.check_motorcycle) ||
       (t == ObjectClassification::PEDESTRIAN && target_object_types.check_pedestrian));

    // If the object type matches any of the target types, add it to the filtered list
    if (is_object_type) {
      filtered_objects.objects.push_back(object);
    }
  }

  // Replace the original objects with the filtered list
  objects = std::move(filtered_objects);

  return;
}

std::pair<std::vector<size_t>, std::vector<size_t>> separateObjectIndicesByLanelets(
  const PredictedObjects & objects, const lanelet::ConstLanelets & target_lanelets)
{
  if (target_lanelets.empty()) {
    return {};
  }

  std::vector<size_t> target_indices;
  std::vector<size_t> other_indices;

  for (size_t i = 0; i < objects.objects.size(); i++) {
    // create object polygon
    const auto & obj = objects.objects.at(i);
    // create object polygon
    const auto obj_polygon = tier4_autoware_utils::toPolygon2d(obj);
    bool is_filtered_object = false;
    for (const auto & llt : target_lanelets) {
      // create lanelet polygon
      const auto polygon2d = llt.polygon2d().basicPolygon();
      if (polygon2d.empty()) {
        // no lanelet polygon
        continue;
      }
      Polygon2d lanelet_polygon;
      for (const auto & lanelet_point : polygon2d) {
        lanelet_polygon.outer().emplace_back(lanelet_point.x(), lanelet_point.y());
      }
      lanelet_polygon.outer().push_back(lanelet_polygon.outer().front());
      // check the object does not intersect the lanelet
      if (!boost::geometry::disjoint(lanelet_polygon, obj_polygon)) {
        target_indices.push_back(i);
        is_filtered_object = true;
        break;
      }
    }

    if (!is_filtered_object) {
      other_indices.push_back(i);
    }
  }

  return std::make_pair(target_indices, other_indices);
}

std::pair<PredictedObjects, PredictedObjects> separateObjectsByLanelets(
  const PredictedObjects & objects, const lanelet::ConstLanelets & target_lanelets)
{
  PredictedObjects target_objects;
  PredictedObjects other_objects;

  const auto [target_indices, other_indices] =
    separateObjectIndicesByLanelets(objects, target_lanelets);

  target_objects.objects.reserve(target_indices.size());
  other_objects.objects.reserve(other_indices.size());

  for (const size_t i : target_indices) {
    target_objects.objects.push_back(objects.objects.at(i));
  }

  for (const size_t i : other_indices) {
    other_objects.objects.push_back(objects.objects.at(i));
  }

  return std::make_pair(target_objects, other_objects);
}

std::vector<PredictedPathWithPolygon> getPredictedPathFromObj(
  const ExtendedPredictedObject & obj, const bool & is_use_all_predicted_path)
{
  if (!is_use_all_predicted_path) {
    const auto max_confidence_path = std::max_element(
      obj.predicted_paths.begin(), obj.predicted_paths.end(),
      [](const auto & path1, const auto & path2) { return path1.confidence < path2.confidence; });
    if (max_confidence_path != obj.predicted_paths.end()) {
      return {*max_confidence_path};
    }
  }

  return obj.predicted_paths;
}

// TODO(Sugahara): should consider delay before departure
std::vector<PoseWithVelocityStamped> createPredictedPath(
  const std::shared_ptr<EgoPredictedPathParams> & ego_predicted_path_params,
  const std::vector<PathPointWithLaneId> & path_points,
  const geometry_msgs::msg::Pose & vehicle_pose, const double current_velocity, size_t ego_seg_idx)
{
  if (path_points.empty()) {
    return {};
  }

  const double min_slow_down_speed = ego_predicted_path_params->min_slow_speed;
  const double acceleration = ego_predicted_path_params->acceleration;
  const double time_horizon = ego_predicted_path_params->time_horizon;
  const double time_resolution = ego_predicted_path_params->time_resolution;

  std::vector<PoseWithVelocityStamped> predicted_path;
  const auto vehicle_pose_frenet =
    convertToFrenetPoint(path_points, vehicle_pose.position, ego_seg_idx);

  for (double t = 0.0; t < time_horizon + 1e-3; t += time_resolution) {
    const double velocity = std::max(current_velocity + acceleration * t, min_slow_down_speed);
    const double length = current_velocity * t + 0.5 * acceleration * t * t;
    const auto pose =
      motion_utils::calcInterpolatedPose(path_points, vehicle_pose_frenet.length + length);
    predicted_path.emplace_back(t, pose, velocity);
  }

  return predicted_path;
}

bool isCentroidWithinLanelets(
  const PredictedObject & object, const lanelet::ConstLanelets & target_lanelets)
{
  if (target_lanelets.empty()) {
    return false;
  }

  const auto & object_pos = object.kinematics.initial_pose_with_covariance.pose.position;
  lanelet::BasicPoint2d object_centroid(object_pos.x, object_pos.y);

  for (const auto & llt : target_lanelets) {
    if (boost::geometry::within(object_centroid, llt.polygon2d().basicPolygon())) {
      return true;
    }
  }

  return false;
}

ExtendedPredictedObject transform(
  const PredictedObject & object, const double safety_check_time_horizon,
  const double safety_check_time_resolution)
{
  ExtendedPredictedObject extended_object;
  extended_object.uuid = object.object_id;
  extended_object.initial_pose = object.kinematics.initial_pose_with_covariance;
  extended_object.initial_twist = object.kinematics.initial_twist_with_covariance;
  extended_object.initial_acceleration = object.kinematics.initial_acceleration_with_covariance;
  extended_object.shape = object.shape;

  const auto obj_velocity = extended_object.initial_twist.twist.linear.x;

  extended_object.predicted_paths.resize(object.kinematics.predicted_paths.size());
  for (size_t i = 0; i < object.kinematics.predicted_paths.size(); ++i) {
    const auto & path = object.kinematics.predicted_paths[i];
    extended_object.predicted_paths[i].confidence = path.confidence;

    // Create path based on time horizon and resolution
    for (double t = 0.0; t < safety_check_time_horizon + 1e-3; t += safety_check_time_resolution) {
      const auto obj_pose = object_recognition_utils::calcInterpolatedPose(path, t);
      if (obj_pose) {
        const auto obj_polygon = tier4_autoware_utils::toPolygon2d(*obj_pose, object.shape);
        extended_object.predicted_paths[i].path.emplace_back(
          t, *obj_pose, obj_velocity, obj_polygon);
      }
    }
  }

  return extended_object;
}

TargetObjectsOnLane createTargetObjectsOnLane(
  const lanelet::ConstLanelets & current_lanes, const std::shared_ptr<RouteHandler> & route_handler,
  const PredictedObjects & filtered_objects, const std::shared_ptr<ObjectsFilteringParams> & params)
{
  const auto & object_lane_configuration = params->object_lane_configuration;
  const bool include_opposite = params->include_opposite_lane;
  const bool invert_opposite = params->invert_opposite_lane;
  const double safety_check_time_horizon = params->safety_check_time_horizon;
  const double safety_check_time_resolution = params->safety_check_time_resolution;

  lanelet::ConstLanelets all_left_lanelets;
  lanelet::ConstLanelets all_right_lanelets;

  // Define lambda functions to update left and right lanelets
  const auto update_left_lanelets = [&](const lanelet::ConstLanelet & target_lane) {
    const auto left_lanelets = route_handler->getAllLeftSharedLinestringLanelets(
      target_lane, include_opposite, invert_opposite);
    all_left_lanelets.insert(all_left_lanelets.end(), left_lanelets.begin(), left_lanelets.end());
  };

  const auto update_right_lanelets = [&](const lanelet::ConstLanelet & target_lane) {
    const auto right_lanelets = route_handler->getAllRightSharedLinestringLanelets(
      target_lane, include_opposite, invert_opposite);
    all_right_lanelets.insert(
      all_right_lanelets.end(), right_lanelets.begin(), right_lanelets.end());
  };

  // Update left and right lanelets for each current lane
  for (const auto & current_lane : current_lanes) {
    update_left_lanelets(current_lane);
    update_right_lanelets(current_lane);
  }

  TargetObjectsOnLane target_objects_on_lane{};
  const auto append_objects_on_lane = [&](auto & lane_objects, const auto & check_lanes) {
    std::for_each(
      filtered_objects.objects.begin(), filtered_objects.objects.end(), [&](const auto & object) {
        if (isCentroidWithinLanelets(object, check_lanes)) {
          lane_objects.push_back(
            transform(object, safety_check_time_horizon, safety_check_time_resolution));
        }
      });
  };

  // TODO(Sugahara): Consider shoulder and other lane objects
  if (object_lane_configuration.check_current_lane && !current_lanes.empty()) {
    append_objects_on_lane(target_objects_on_lane.on_current_lane, current_lanes);
  }
  if (object_lane_configuration.check_left_lane && !all_left_lanelets.empty()) {
    append_objects_on_lane(target_objects_on_lane.on_left_lane, all_left_lanelets);
  }
  if (object_lane_configuration.check_right_lane && !all_right_lanelets.empty()) {
    append_objects_on_lane(target_objects_on_lane.on_right_lane, all_right_lanelets);
  }

  return target_objects_on_lane;
}

}  // namespace behavior_path_planner::utils::path_safety_checker
