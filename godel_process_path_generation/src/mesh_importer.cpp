/*
 * Software License Agreement (Apache License)
 *
 * Copyright (c) 2014, Southwest Research Institute
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
 */
/*
 * mesh_importer.cpp
 *
 *  Created on: May 5, 2014
 *      Author: Dan Solomon
 */

#include <ros/ros.h>
#include <boost/bimap.hpp>
#include <pcl/geometry/triangle_mesh.h>
#include "godel_process_path_generation/get_boundary.h"
#include <pcl/ModelCoefficients.h>
#include <pcl/point_types.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/common/impl/centroid.hpp>
#include "godel_process_path_generation/mesh_importer.h"

using Eigen::Vector3d;
using Eigen::Vector4d;

namespace godel_process_path
{

bool MeshImporter::calculateBoundaryData(const pcl::PolygonMesh &input_mesh)
{
  typedef pcl::geometry::TriangleMesh<pcl::geometry::DefaultMeshTraits<> > Mesh;

  plane_frame_.setIdentity();
  boundaries_.clear();

  Cloud::Ptr points(new Cloud);
  pcl::fromPCLPointCloud2(input_mesh.cloud,*points);


  /* Find plane coefficients from point cloud data.
   * Find centroid of point cloud as origin of local coordinate system.
   * Create local coordinate frame for plane.
   */
  Eigen::Hyperplane<double, 3> hplane;
  if (!computePlaneCoefficients(points, hplane.coeffs()))
  {
    ROS_WARN("Could not compute plane coefficients");
    return false;
  }
  if (verbose_)
  {
    ROS_INFO_STREAM("Normal: " << hplane.coeffs().transpose());
  }

  Eigen::Vector4d centroid4;
  pcl::compute3DCentroid(*points, centroid4);
  computeLocalPlaneFrame(hplane, centroid4.head(3));


  /* Use pcl::geometry::TriangleMesh to extract boundaries.
   * Project boundaries to local plane, and add to boundaries_ list.
   * Note: External boundary is CCW ordered, internal boundaries are CW ordered.
   */
  Mesh mesh;
  typedef boost::bimap<uint32_t, Mesh::VertexIndex> MeshIndexMap;
  MeshIndexMap mesh_index_map;
  for (size_t ii = 0; ii < input_mesh.polygons.size(); ++ii)
  {
    const std::vector<uint32_t> &vertices = input_mesh.polygons.at(ii).vertices;
    if (vertices.size() != 3)
    {
      ROS_ERROR_STREAM("Found polygon with " << vertices.size() << " sides, only triangle mesh supported!");
      return false;
    }
    Mesh::VertexIndices vi;
    for (std::vector<uint32_t>::const_iterator vidx = vertices.begin(), viend = vertices.end(); vidx != viend; ++vidx)
    {
//      mesh_index_map2.left.count
      if (!mesh_index_map.left.count(*vidx))
      {
        mesh_index_map.insert(MeshIndexMap::value_type(*vidx, mesh.addVertex()));
      }
      vi.push_back(mesh_index_map.left.at(*vidx));
    }
    mesh.addFace(vi.at(0), vi.at(1), vi.at(2));
  }

  // Extract edges from mesh. Project to plane and add to boundaries_.
  std::vector<Mesh::HalfEdgeIndices> boundary_he_indices;
  pcl_godel::geometry::getBoundBoundaryHalfEdges(mesh, boundary_he_indices);

  // For each boundary, project boundary points onto plane and add to boundaries_
  Eigen::Affine3d plane_inverse = plane_frame_.inverse();
  for (std::vector<Mesh::HalfEdgeIndices>::const_iterator boundary = boundary_he_indices.begin(), b_end = boundary_he_indices.end();
       boundary != b_end; ++boundary)
  {
    PolygonBoundary pbound;
    for (Mesh::HalfEdgeIndices::const_iterator edge = boundary->begin(), edge_end = boundary->end();
         edge != edge_end; ++edge)
    {
      Cloud::PointType cloudpt = points->points.at(mesh_index_map.right.at(mesh.getOriginatingVertexIndex(*edge))); // pt on boundary
      Eigen::Vector3d projected_pt = hplane.projection(Eigen::Vector3d(cloudpt.x, cloudpt.y, cloudpt.z));        // pt projected onto plane
      Eigen::Vector3d plane_pt = plane_inverse * projected_pt;                                                   // pt in plane frame
      ROS_ASSERT(std::abs(plane_pt(2)) < .001);
      pbound.push_back(PolygonPt(plane_pt(0), plane_pt(1)));
    }

    boundaries_.push_back(pbound);
  }

  return true;
}

void MeshImporter::computeLocalPlaneFrame(const Eigen::Hyperplane<double, 3> &plane, const Vector3d &centroid)
{
  Eigen::Vector3d origin = plane.projection(centroid);       // Project centroid onto plane

  // Check if z_axis (plane normal) is closely aligned with world x_axis:
  // If not, construct transform rotation from X,Z axes. Otherwise, use Y,Z axes.
  const Eigen::Vector3d& plane_normal = plane.coeffs().head(3);
  if (std::abs(plane_normal.dot(Vector3d::UnitX())) < 0.8)
  {
    Eigen::Vector3d x_axis = plane.projection(origin + Eigen::Vector3d::UnitX());
    plane_frame_.matrix().col(0).head(3) = x_axis.normalized();
    plane_frame_.matrix().col(2).head(3) = plane_normal.normalized();
    plane_frame_.matrix().col(1).head(3) = (plane_normal.normalized().cross(x_axis.normalized())).normalized();
  }
  else
  {
    Eigen::Vector3d y_axis = plane.projection(origin + Eigen::Vector3d::UnitY());
    plane_frame_.matrix().col(1).head(3) = y_axis.normalized();
    plane_frame_.matrix().col(2).head(3) = plane_normal.normalized();
    plane_frame_.matrix().col(0).head(3) = (y_axis.normalized().cross(plane_normal.normalized())).normalized();
  }
  plane_frame_.translation() = origin;
}

bool MeshImporter::computePlaneCoefficients(Cloud::ConstPtr cloud, Eigen::Vector4d &output)
{
  pcl::ModelCoefficients coefficients;
  pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
  // Create the segmentation object
  pcl::SACSegmentation<Cloud::PointType> seg;
  // Optional
  seg.setOptimizeCoefficients (true);
  // Mandatory
  seg.setModelType (pcl::SACMODEL_PLANE);
  seg.setMethodType (pcl::SAC_RANSAC);
  seg.setDistanceThreshold (0.01);
  const Cloud::PointType &p0 = cloud->points.at(0);
  Eigen::Vector3f expected_normal(p0.normal_x, p0.normal_y, p0.normal_z);
  seg.setAxis(expected_normal);
  seg.setEpsAngle(0.5);

  seg.setInputCloud (cloud);
  seg.segment (*inliers, coefficients);
  ROS_ASSERT(coefficients.values.size() == 4);

  // Check that points match a plane fit
  if (static_cast<double>(inliers->indices.size())/static_cast<double>(cloud->height * cloud->width) < 0.9)
  {
    ROS_WARN("Less than 90%% of points included in plane fit.");
    return false;
  }

  // Check that normal is aligned with pointcloud data
  Eigen::Vector3f actual_normal(coefficients.values.at(0), coefficients.values.at(1), coefficients.values.at(2));
  if (actual_normal.dot(expected_normal) < 0.0)
  {
    ROS_WARN("Flipping RANSAC plane normal");
    actual_normal *= -1;
  }
  if (actual_normal.dot(expected_normal) < std::cos(seg.getEpsAngle()))
  {
    ROS_WARN("RANSAC plane normal out of tolerance! cosines: %f / %f", actual_normal.dot(expected_normal), std::cos(seg.getEpsAngle()));
    return false;
  }

  output(0) = actual_normal(0);
  output(1) = actual_normal(1);
  output(2) = actual_normal(2);
  output(3) = coefficients.values.at(3);
  return true;
}

} /* namespace godel_process_path */
