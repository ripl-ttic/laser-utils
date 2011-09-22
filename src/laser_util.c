/*
 * laser_utils.c
 *
 *  Created on: Jun 27, 2009
 *      Author: abachrac
 */
#include "laser_util.h"
#include <bot_param/param_util.h>
#define LASER_MAX_SENSOR_RANGE_DEFAULT 30.0 /* meters */
#define LASER_MIN_SENSOR_RANGE_DEFAULT 0.15 /* meters */
#define LASER_FREQUENCY_DEFAULT        40   /* meters */

Laser_projector * laser_projector_new(BotParam *param, BotFrames * frames, const char * laser_name, int project_height)
{
  Laser_projector * self = (Laser_projector *) calloc(1, sizeof(Laser_projector));
  self->param = param;
  self->bot_frames = frames;
  self->laser_name = strdup(laser_name);
  self->project_height = project_height;

  self->coord_frame = bot_param_get_planar_lidar_coord_frame(self->param, self->laser_name);

  char param_prefix[1024]; /* param file path */
  bot_param_get_planar_lidar_prefix(NULL, self->laser_name, param_prefix, sizeof(param_prefix));

  char key[1024];
  sprintf(key, "%s.max_range", param_prefix);
  if (0 != bot_param_get_double(self->param, key, &self->max_range)) {
    fprintf(stderr, "Error: Missing or funny max_range configuration parameter "
        "for planar LIDAR configuration key: '%s'\n", key);
    self->max_range = LASER_MAX_SENSOR_RANGE_DEFAULT;
  }

  sprintf(key, "%s.min_range", param_prefix);
  if (0 != bot_param_get_double(self->param, key, &self->min_range)) {
    fprintf(stderr, "Error: Missing or funny min_range configuration parameter "
        "for planar LIDAR configuration key: '%s'\n", key);
    self->min_range = LASER_MIN_SENSOR_RANGE_DEFAULT;
  }

  sprintf(key, "%s.down_region", param_prefix);
  if (2 != bot_param_get_int_array(self->param, key, self->heightDownRegion, 2)) {
    fprintf(stderr, "Error: Missing or funny down region parameter "
        "for planar LIDAR configuration key: '%s'\n", key);
    self->heightDownRegion[0] = -1;
    self->heightDownRegion[1] = -1;
  }

  sprintf(key, "%s.up_region", param_prefix);
  if (2 != bot_param_get_int_array(self->param, key, self->heightUpRegion, 2)) {
    fprintf(stderr, "Error: Missing or funny up region parameter "
        "for planar LIDAR configuration key: '%s'\n", key);
    self->heightUpRegion[0] = -1;
    self->heightUpRegion[1] = -1;
  }

  sprintf(key, "%s.surround_region", param_prefix);
  if (2 != bot_param_get_int_array(self->param, key, self->surroundRegion, 2)) {
    fprintf(stderr, "Error: Missing or funny surround region parameter "
        "for planar LIDAR configuration key: '%s'\n", key);
    self->surroundRegion[0] = 0;
    self->surroundRegion[1] = 1e6;
  }

  sprintf(key, "%s.frequency", param_prefix);
  if (0 != bot_param_get_double(self->param, key, &self->laser_frequency)) {
    fprintf(stderr, "Error: Missing or funny laser_frequency parameter "
        "for planar LIDAR configuration key: '%s'\n", key);
    self->laser_frequency = LASER_FREQUENCY_DEFAULT;
  }

  sprintf(key, "%s.laser_type", param_prefix);
  if (0 != bot_param_get_str(self->param, key, &self->laser_type)) {
    fprintf(stderr, "Error: Missing or funny laser_type parameter "
        "for planar LIDAR configuration key: '%s'\n", key);
    self->laser_type = strdup("HOKUYO_UTM");
  }

  return self;
}
void laser_projector_destroy(Laser_projector * to_destroy)
{
  free(to_destroy->laser_name);
  free(to_destroy->coord_frame);
  free(to_destroy->laser_type);
  free(to_destroy);
}

int laser_update_projected_scan(Laser_projector * projector, laser_projected_scan * proj_scan, const char * dest_frame)
{
  double zeros[3] = { 0 };
  return laser_update_projected_scan_with_motion(projector, proj_scan, dest_frame, zeros, zeros);
}

int laser_update_projected_scan_with_motion(Laser_projector * projector, laser_projected_scan * proj_scan,
    const char * dest_frame, const double laser_angular_rate[3], const double laser_velocity[3])
{
  if (!bot_frames_get_trans_with_utime(projector->bot_frames, projector->coord_frame, dest_frame, proj_scan->utime,
      &proj_scan->origin)) {
    return proj_scan->projection_status;
  }

  int64_t latest_trans_timestamp;
  bot_frames_get_trans_latest_timestamp(projector->bot_frames, projector->coord_frame, dest_frame,
      &latest_trans_timestamp);

  bot_frames_get_trans_with_utime(projector->bot_frames, "body", bot_frames_get_root_name(projector->bot_frames),
      proj_scan->utime, &proj_scan->body);

  if (proj_scan->projection_status == 0 && (latest_trans_timestamp - 100000) > proj_scan->utime) {
    proj_scan->projection_status = -1;
  }
  else if (latest_trans_timestamp > proj_scan->utime) {
    proj_scan->projection_status = 1;
  }
  else
    proj_scan->projection_status = 0;

  proj_scan->numValidPoints = 0;

  double aveRange = 0;
  double aveRangeSq = 0;
  double surroundCount = 0;

  //values for motion correcting the scan
  double timestep = (proj_scan->rawScan->radstep / (2 * M_PI)) / proj_scan->projector->laser_frequency;
  double scan_time = proj_scan->rawScan->nranges * timestep;
  double dt;
  BotTrans laser_hit_time_to_laser_current_time;
  BotTrans laser_hit_time_to_dest;

  /* convert the range data to the local frame */
  for (int i = 0; i < proj_scan->npoints; i++) {
    double range = proj_scan->rawScan->ranges[i];
    double theta = proj_scan->rawScan->rad0 + proj_scan->rawScan->radstep * i;

    /* point is invalid if range exceeds maximum sensor range */
    if (range >= projector->max_range)
      proj_scan->invalidPoints[i] = 1;
    else if (range <= projector->min_range)
      proj_scan->invalidPoints[i] = 2;
    double s, c;
    bot_fasttrig_sincos(theta, &s, &c);

    double sensor_xyz[3];
    /* point in sensor coordinates */

    if (projector->surroundRegion[0] <= i && i <= projector->surroundRegion[1]) {
      sensor_xyz[0] = c * range;
      sensor_xyz[1] = s * range;
      sensor_xyz[2] = 0;
      aveRange += range;
      aveRangeSq += range * range;
      surroundCount++;
    }
    else if (projector->project_height && projector->heightDownRegion[0] <= i && i <= projector->heightDownRegion[1]) {
      sensor_xyz[0] = 0;
      sensor_xyz[1] = 0;
      sensor_xyz[2] = -range;
    }
    else if (projector->project_height && projector->heightUpRegion[0] <= i && i <= projector->heightUpRegion[1]) {
      sensor_xyz[0] = 0;
      sensor_xyz[1] = 0;
      sensor_xyz[2] = range;
    }
    else {
      sensor_xyz[0] = 0;
      sensor_xyz[1] = 0;
      sensor_xyz[2] = 0;
      proj_scan->invalidPoints[i] = 3;
    }

    if (!proj_scan->invalidPoints[i]) {
      proj_scan->numValidPoints++;
    }
    /* convert to local frame */

    //dt will be negative since we start farthest back in time (points[0]) and step forward to the last point
    //todo: check for 0 velocities and don't do set_from_velocities step to save computation
    dt = -scan_time + timestep * i;
    bot_trans_set_from_velocities(&laser_hit_time_to_laser_current_time, laser_angular_rate, laser_velocity, dt);
    bot_trans_apply_trans_to(&proj_scan->origin, &laser_hit_time_to_laser_current_time, &laser_hit_time_to_dest);

    bot_trans_apply_vec(&laser_hit_time_to_dest, sensor_xyz, point3d_as_array(&proj_scan->points[i]));
  }
  aveRange /= surroundCount;
  aveRangeSq /= surroundCount;
  proj_scan->aveSurroundRange = aveRange;
  proj_scan->stddevSurroundRange = sqrt(aveRangeSq - aveRange * aveRange);
  return proj_scan->projection_status;
}

void laser_decimate_projected_scan(laser_projected_scan * lscan, int beam_skip, double spatial_decimation)
{
  int lastAdd = -1e6;
  for (int i = 0; i < lscan->npoints; i++) {
    if (lscan->invalidPoints[i])
      continue;
    if ((i - lastAdd) > beam_skip
        || bot_vector_dist_3d(point3d_as_array(&lscan->points[i]), point3d_as_array(&lscan->points[lastAdd]))
            > spatial_decimation
        || bot_vector_dist_3d(point3d_as_array(&lscan->points[i]), lscan->origin.trans_vec)
            > (lscan->aveSurroundRange + 1.8 * lscan->stddevSurroundRange) ||
        i < lscan->projector->surroundRegion[0]
        || i > lscan->projector->surroundRegion[1]) {
      lastAdd = i;
    }
    else {
      lscan->invalidPoints[i] = 3;
      lscan->numValidPoints--;
    }
  }
}

laser_projected_scan *laser_create_projected_scan_from_planar_lidar_with_motion(Laser_projector * projector,
    const bot_core_planar_lidar_t *msg, const char * dest_frame, const double laser_angular_rate[3],
    const double laser_velocity[3])
{
  laser_projected_scan * proj_scan = (laser_projected_scan *) calloc(1, sizeof(laser_projected_scan));
  proj_scan->npoints = msg->nranges;
  proj_scan->numValidPoints = 0;
  proj_scan->utime = msg->utime;
  proj_scan->projector = projector;
  proj_scan->rawScan = bot_core_planar_lidar_t_copy(msg);

  proj_scan->points = (point3d_t*) calloc(proj_scan->npoints, sizeof(point3d_t));
  g_assert(proj_scan->points);
  proj_scan->invalidPoints = (uint8_t *) calloc(proj_scan->npoints, sizeof(char));
  g_assert(proj_scan->invalidPoints);

  if (!bot_frames_have_trans(projector->bot_frames, projector->coord_frame, dest_frame)) {
    laser_destroy_projected_scan(proj_scan);
    return NULL;
  }
  else {
    if (laser_update_projected_scan_with_motion(projector, proj_scan, dest_frame, laser_angular_rate, laser_velocity)
        == -1) { //scan is arriving way late
      laser_destroy_projected_scan(proj_scan);
      return NULL;
    }
  }

  return proj_scan;
}

laser_projected_scan *laser_create_projected_scan_from_planar_lidar(Laser_projector * projector,
    const bot_core_planar_lidar_t *msg, const char * dest_frame)
{
  double zeros[3] = { 0 };
  return laser_create_projected_scan_from_planar_lidar_with_motion(projector, msg, dest_frame, zeros, zeros);
}

void laser_destroy_projected_scan(laser_projected_scan * proj_scan)
{
  if (proj_scan->points != NULL
  )
    free(proj_scan->points);
  if (proj_scan->invalidPoints != NULL
  )
    free(proj_scan->invalidPoints);
  if (proj_scan->rawScan != NULL
  )
    bot_core_planar_lidar_t_destroy(proj_scan->rawScan);
  free(proj_scan);
}

