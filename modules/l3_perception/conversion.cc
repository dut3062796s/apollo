/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 */

#include <vector>

#include "modules/l3_perception/conversion.h"
#include "modules/l3_perception/l3_perception_gflags.h"
#include "modules/l3_perception/l3_perception_util.h"

/**
 * @namespace apollo::l3_perception::conversion
 * @brief apollo::l3_perception
 */
namespace apollo {
namespace l3_perception {
namespace conversion {

using ::apollo::l3_perception::GetAngleFromQuaternion;
using ::apollo::l3_perception::FillPerceptionPolygon;
using ::apollo::l3_perception::GetDefaultObjectLength;
using ::apollo::l3_perception::GetDefaultObjectWidth;
using ::apollo::perception::Point;

PerceptionObstacles MobileyeToPerceptionObstacles(
    const Mobileye& mobileye, const LocalizationEstimate& localization) {
  PerceptionObstacles obstacles;
  double adc_x = localization.pose().position().x();
  double adc_y = localization.pose().position().y();
  double adc_z = localization.pose().position().z();
  auto adc_quaternion = localization.pose().orientation();
  double adc_vx = localization.pose().linear_velocity().x();
  double adc_vy = localization.pose().linear_velocity().y();
  double adc_velocity = std::sqrt(adc_vx * adc_vx + adc_vy * adc_vy);

  double adc_theta = GetAngleFromQuaternion(adc_quaternion);

  for (int index = 0; index < mobileye.details_738().num_obstacles() &&
                      index < mobileye.details_739_size();
       ++index) {
    auto* mob = obstacles.add_perception_obstacle();
    const auto& data_739 = mobileye.details_739(index);
    int mob_id = data_739.obstacle_id();
    double mob_pos_x = data_739.obstacle_pos_x();
    double mob_pos_y = -data_739.obstacle_pos_y();
    double mob_vel_x = data_739.obstacle_rel_vel_x();
    int mob_type = data_739.obstacle_type();

    double mob_l = GetDefaultObjectLength(mob_type);

    double mob_w = 0.0;
    if (mobileye.details_73a_size() <= index) {
      mob_w = GetDefaultObjectWidth(mob_type);
    } else {
      mob_w = mobileye.details_73a(index).obstacle_width();
    }

    // TODO(lizh): calibrate mobileye and make those consts FLAGS
    mob_pos_x += FLAGS_mobileye_pos_adjust;  // offset: imu <-> mobileye
    mob_pos_x += mob_l / 2.0;  // make x the middle point of the vehicle.

    Point sl_point;
    sl_point.set_x(mob_pos_x);
    sl_point.set_y(mob_pos_y);
    Point xy_point = SLtoXY(sl_point, adc_theta);

    double converted_x = adc_x + xy_point.x();
    double converted_y = adc_y + xy_point.y();
    xy_point.set_x(converted_x);
    xy_point.set_y(converted_y);
    xy_point.set_z(adc_z);
    mob->set_theta(GetNearestLaneHeading(xy_point));
    // TODO(rongqiqiu): consider more accurate speed computation
    double converted_speed = adc_velocity + mob_vel_x;
    double converted_vx = converted_speed * std::cos(mob->theta());
    double converted_vy = converted_speed * std::sin(mob->theta());

    mob->set_id(mob_id);
    mob->mutable_position()->set_x(converted_x);
    mob->mutable_position()->set_y(converted_y);

    switch (mob_type) {
      case 0:
      case 1: {
        mob->set_type(PerceptionObstacle::VEHICLE);  // VEHICLE
        break;
      }
      case 2:
      case 4: {
        mob->set_type(PerceptionObstacle::BICYCLE);  // BIKE
        break;
      }
      case 3: {
        mob->set_type(PerceptionObstacle::PEDESTRIAN);  // PED
        break;
      }
      default: {
        mob->set_type(PerceptionObstacle::UNKNOWN);  // UNKNOWN
        break;
      }
    }

    mob->mutable_velocity()->set_x(converted_vx);
    mob->mutable_velocity()->set_y(converted_vy);
    mob->set_length(mob_l);
    mob->set_width(mob_w);
    mob->set_height(3.0);

    mob->clear_polygon_point();
    double mid_x = converted_x;
    double mid_y = converted_y;
    double mid_z = adc_z;
    double heading = mob->theta();

    FillPerceptionPolygon(mob, mid_x, mid_y, mid_z, mob_l, mob_w, mob->height(),
                          heading);

    mob->set_confidence(0.75);
  }

  obstacles.mutable_header()->CopyFrom(mobileye.header());

  return obstacles;
}

RadarObstacles DelphiToRadarObstacles(
    const DelphiESR& delphi_esr, const LocalizationEstimate& localization,
    const RadarObstacles& last_radar_obstacles) {
  RadarObstacles obstacles;

  const double last_timestamp =
      last_radar_obstacles.header().timestamp_sec();
  const double current_timestamp = delphi_esr.header().timestamp_sec();

  // assign motion power from 540
  std::vector<apollo::drivers::Esr_trackmotionpower_540::Motionpower>
      motionpowers(64);
  for (const auto& esr_trackmotionpower_540 :
       delphi_esr.esr_trackmotionpower_540()) {
    const int& can_tx_track_can_id_group =
        esr_trackmotionpower_540.can_tx_track_can_id_group();
    for (int index = 0; index < (can_tx_track_can_id_group < 9 ? 7 : 1);
         ++index) {
      motionpowers[can_tx_track_can_id_group * 7 + index].CopyFrom(
          esr_trackmotionpower_540.can_tx_track_motion_power(index));
    }
  }

  const auto adc_pos = localization.pose().position();
  const auto adc_vel = localization.pose().linear_velocity();
  const auto adc_quaternion = localization.pose().orientation();
  const double adc_theta = GetAngleFromQuaternion(adc_quaternion);

  for (int index = 0; index < delphi_esr.esr_track01_500_size(); ++index) {
    const auto& data_500 = delphi_esr.esr_track01_500(index);

    // ignore invalid target
    if (data_500.can_tx_track_status() ==
        ::apollo::drivers::Esr_track01_500::CAN_TX_TRACK_STATUS_NO_TARGET) {
      continue;
    }

    RadarObstacle rob;

    rob.set_id(index);
    rob.set_rcs(static_cast<double>(motionpowers[index].can_tx_track_power()) -
                10.0);
    rob.set_theta(adc_theta);
    rob.set_length(GetDefaultObjectLength(4));
    rob.set_width(GetDefaultObjectWidth(4));
    rob.set_height(3.0);

    const double range = data_500.can_tx_track_range();
    const double angle = data_500.can_tx_track_angle() * L3_PI / 180.0;
    Point relative_pos_sl;
    relative_pos_sl.set_x(
        range * std::cos(angle) +
        FLAGS_delphi_esr_pos_adjust +  // offset: imu <-> mobileye
        rob.length() / 2.0);           // make x the middle point of the vehicle
    relative_pos_sl.set_y(range * std::sin(angle));
    rob.mutable_relative_position()->CopyFrom(relative_pos_sl);

    Point relative_pos_xy = SLtoXY(relative_pos_sl, adc_theta);
    Point absolute_pos;
    absolute_pos.set_x(adc_pos.x() + relative_pos_xy.x());
    absolute_pos.set_y(adc_pos.y() + relative_pos_xy.y());
    absolute_pos.set_z(adc_pos.z());
    rob.mutable_absolute_position()->CopyFrom(absolute_pos);

    const double range_vel = data_500.can_tx_track_range_rate();
    const double lateral_vel = data_500.can_tx_track_lat_rate();
    rob.mutable_relative_velocity()->set_x(range_vel * std::cos(angle) -
                                           lateral_vel * std::sin(angle));
    rob.mutable_relative_velocity()->set_y(range_vel * std::sin(angle) +
                                           lateral_vel * std::cos(angle));

    const auto iter =
        last_radar_obstacles.radar_obstacle().find(index);
    if (iter == last_radar_obstacles.radar_obstacle().end()) {
      rob.set_count(0);
      rob.set_movable(false);
      rob.set_moving_frames_count(0);
    } else {
      rob.set_count(iter->second.count() + 1);
      rob.set_movable(iter->second.movable());
    }

    Point absolute_vel;
    const auto iter_front =
        last_radar_obstacles.radar_obstacle().find(index);
    if (iter_front == last_radar_obstacles.radar_obstacle().end()) {
      // new in the current frame
      absolute_vel.set_x(0.0);
      absolute_vel.set_y(0.0);
      absolute_vel.set_z(0.0);
    } else {
      // appeared in the last frame
      absolute_vel.set_x(
          (absolute_pos.x() - iter_front->second.absolute_position().x()) /
          (current_timestamp - last_timestamp));
      absolute_vel.set_y(
          (absolute_pos.y() - iter_front->second.absolute_position().y()) /
          (current_timestamp - last_timestamp));
      absolute_vel.set_z(0.0);
    }
    rob.mutable_absolute_velocity()->CopyFrom(absolute_vel);

    double v_heading = std::atan2(rob.absolute_velocity().y(), rob.absolute_velocity().x());
    if (Speed(rob.absolute_velocity()) > 6.7 && std::abs(v_heading - rob.theta()) < 1.5) {
      rob.set_moving_frames_count(iter->second.moving_frames_count() + 1);
    } else {
      rob.set_moving_frames_count(0);
    }
    if (rob.moving_frames_count() >= 5) {
      rob.set_movable(true);
    }
    (*obstacles.mutable_radar_obstacle())[index] = rob;
  }

  obstacles.mutable_header()->CopyFrom(delphi_esr.header());
  return obstacles;
}

PerceptionObstacles RadarObstaclesToPerceptionObstacles(
    const RadarObstacles& radar_obstacles) {
  PerceptionObstacles obstacles;

  for (const auto& iter : radar_obstacles.radar_obstacle()) {
    auto* pob = obstacles.add_perception_obstacle();
    const auto& radar_obstacle = iter.second;

    pob->set_id(radar_obstacle.id() + 1000);

    pob->set_type(PerceptionObstacle::UNKNOWN);  // UNKNOWN
    pob->set_length(radar_obstacle.length());
    pob->set_width(radar_obstacle.width());
    pob->set_height(radar_obstacle.height());

    pob->mutable_position()->CopyFrom(radar_obstacle.absolute_position());
    pob->mutable_velocity()->CopyFrom(radar_obstacle.absolute_velocity());

    Point xy_point;
    xy_point.set_x(pob->position().x());
    xy_point.set_y(pob->position().y());
    xy_point.set_z(pob->position().z());
    pob->set_theta(GetNearestLaneHeading(xy_point));

    // create polygon
    pob->clear_polygon_point();
    double mid_x = pob->position().x();
    double mid_y = pob->position().y();
    double mid_z = pob->position().z();
    double heading = pob->theta();

    FillPerceptionPolygon(pob, mid_x, mid_y, mid_z, pob->length(), pob->width(),
                          pob->height(), heading);

    pob->set_confidence(0.5);
  }

  obstacles.mutable_header()->CopyFrom(radar_obstacles.header());

  return obstacles;
}

}  // namespace conversion
}  // namespace l3_perception
}  // namespace apollo