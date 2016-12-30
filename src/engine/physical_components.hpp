/* Copyright (C) 2016, Nikolai Wuttke. All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "base/spatial_types.hpp"
#include "engine/base_components.hpp"


namespace rigel { namespace engine {


namespace components {

struct Physical {
  Physical(
    const base::Point<float> velocity,
    const bool gravityAffected,
    const bool canStepUpStairs = false
  )
    : mVelocity(velocity)
    , mGravityAffected(gravityAffected)
    , mCanStepUpStairs(canStepUpStairs)
  {
  }

  base::Point<float> mVelocity;
  bool mGravityAffected;
  bool mCanStepUpStairs;
};


/** Marker component which is added to all entities that had a collision with
 * the level geometry on the last physics update.
 */
struct CollidedWithWorld {};


/** Marks an entity to participate in world collision
 *
 * Other Physical entities will collide against the bounding box of any
 * SolidBody entity as if it were part of the world.
 * */
struct SolidBody {};

}


components::BoundingBox toWorldSpace(
  const components::BoundingBox& bbox, const base::Vector& entityPosition);

}}