/*
 *  Copyright 2013 Néstor Morales Hernández <nestor@isaatc.ull.es>
 * 
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */


#include "stixelstracker.h"

#include "video_input/MetricStereoCamera.hpp"
#include "video_input/MetricCamera.hpp"

#include <pcl/ModelCoefficients.h>
#include <pcl/point_types.h>
#include <pcl/io/pcd_io.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/features/normal_3d.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_clusters.h>
#include <boost/graph/graph_concepts.hpp>

#include <lemon/matching.h>
#include <lemon/smart_graph.h>

#include "utils.h"

using namespace std;
using namespace stixel_world;

const float MIN_FLOAT_DISPARITY = 0.8f;


StixelsTracker::StixelsTracker::StixelsTracker(const boost::program_options::variables_map& options, 
                                               const MetricStereoCamera& camera, int stixels_width,
                                               boost::shared_ptr<PolarCalibration> p_polarCalibration) :
                                               DummyStixelMotionEstimator(options, camera, stixels_width),
                                               mp_polarCalibration(p_polarCalibration)
{ 
    m_stixelsPolarDistMatrix = Eigen::MatrixXf::Zero( motion_cost_matrix.rows(), motion_cost_matrix.cols() ); // Matrix is initialized with 0.
    m_polarSADMatrix = Eigen::MatrixXf::Zero( motion_cost_matrix.rows(), motion_cost_matrix.cols() ); // Matrix is initialized with 0.
    m_denseTrackingMatrix = Eigen::MatrixXf::Zero( motion_cost_matrix.rows(), motion_cost_matrix.cols() ); // Matrix is initialized with 0.
    compute_maximum_pixelwise_motion_for_stixel_lut();
    
    m_sad_factor = 0.3f;
    m_height_factor = 0.0f;
    m_polar_dist_factor = 0.0f;
    m_polar_sad_factor = 0.0f;
    m_dense_tracking_factor = 0.7f;
    
    m_minAllowedObjectWidth = 0.3;
    m_minDistBetweenClusters = 0.3;
    
    m_minPolarSADForBeingStatic = 10;
    
    m_useGraphs = true;
    
    mp_denseTracker.reset(new dense_tracker::DenseTracker());
}

void StixelsTracker::set_motion_cost_factors(const float& sad_factor, const float& height_factor, 
                                             const float& polar_dist_factor, const float & polar_sad_factor,
                                             const float& dense_tracking_factor, const bool & useGraphs)
{
    if ((sad_factor + height_factor + polar_dist_factor + polar_sad_factor + dense_tracking_factor) == 1.0) {
        m_sad_factor = sad_factor;
        m_height_factor = height_factor;
        m_polar_dist_factor = polar_dist_factor;
        m_polar_sad_factor = polar_sad_factor;
        m_dense_tracking_factor = dense_tracking_factor;
    } else {
        cerr << "The sum of motion cost factors should be 1!!!" << endl;
    }
    
    m_useGraphs = useGraphs;
}

void StixelsTracker::transform_stixels_polar()
{
    cv::Mat mapXprev, mapYprev, mapXcurr, mapYcurr;
    mp_polarCalibration->getInverseMaps(mapXprev, mapYprev, 1);
    mp_polarCalibration->getInverseMaps(mapXcurr, mapYcurr, 2);
    
    m_previous_stixels_polar.clear();
    m_current_stixels_polar.clear();
    
    m_previous_stixels_polar.resize(previous_stixels_p->size());
    m_current_stixels_polar.resize(current_stixels_p->size());
    
    copy(previous_stixels_p->begin(), previous_stixels_p->end(), m_previous_stixels_polar.begin());
    copy(current_stixels_p->begin(), current_stixels_p->end(), m_current_stixels_polar.begin());
    
    for (stixels_t::iterator it = m_previous_stixels_polar.begin(); it != m_previous_stixels_polar.end(); it++) {
        const cv::Point2d newPos(mapXprev.at<float>(it->bottom_y, it->x),
                                 mapYprev.at<float>(it->bottom_y, it->x));
        it->x = newPos.x;
        it->bottom_y = newPos.y;
    }
    
    for (stixels_t::iterator it = m_current_stixels_polar.begin(); it != m_current_stixels_polar.end(); it++) {
        const cv::Point2d newPos(mapXcurr.at<float>(it->bottom_y, it->x),
                                 mapYcurr.at<float>(it->bottom_y, it->x));
        it->x = newPos.x;
        it->bottom_y = newPos.y;
    }
}

inline
cv::Point2d StixelsTracker::get_polar_point(const cv::Mat& mapX, const cv::Mat& mapY, const Stixel & stixel, const bool bottom)
{
    if (bottom)
        return cv::Point2d(mapX.at<float>(stixel.bottom_y, stixel.x),
                       mapY.at<float>(stixel.bottom_y, stixel.x));
    else
        return cv::Point2d(mapX.at<float>(stixel.top_y, stixel.x),
                           mapY.at<float>(stixel.top_y, stixel.x));
}

inline
cv::Point2d StixelsTracker::get_polar_point(const cv::Mat& prevMapX, const cv::Mat& prevMapY, 
                                            const cv::Mat& currPolar2LinearX, const cv::Mat& currPolar2LinearY, const Stixel& stixel)
{
    const cv::Point2d polarPoint(prevMapX.at<float>(stixel.bottom_y, stixel.x),
                                 prevMapY.at<float>(stixel.bottom_y, stixel.x));
    
    if (polarPoint == cv::Point2d(-1, -1))
        return polarPoint;
    
    return cv::Point2d(currPolar2LinearX.at<float>(polarPoint.y, polarPoint.x),
                       currPolar2LinearY.at<float>(polarPoint.y, polarPoint.x));
}


inline
cv::Point2d StixelsTracker::get_polar_point(const cv::Mat& mapX, const cv::Mat& mapY, const cv::Point2d & point)
{
    return cv::Point2d(mapX.at<float>(point.y, point.x),
                       mapY.at<float>(point.y, point.x));
}

void StixelsTracker::updateDenseTracker(const cv::Mat & frame)
{
    if (m_dense_tracking_factor != 0.0f)
        mp_denseTracker->compute(frame);
}

void StixelsTracker::compute()
{
//     DummyStixelMotionEstimator::compute();
//     updateTracker();
    
//     compute_motion_cost_matrix();
//     compute_motion_v1();
//     updateTracker();
    
//     compute_static_stixels();
    compute_motion_cost_matrix();
    if (m_useGraphs)
        computeMotionWithGraphs();
    else
        compute_motion();
//     update_stixel_tracks_image();
    updateTracker();
//     estimate_stixel_direction();
//     getClusters();
    
    return;
}

void StixelsTracker::compute_motion_cost_matrix()
{    
    
    const double & startWallTime = omp_get_wtime();
    
    const float maximum_depth_difference = 1.0;
    
    const float maximum_allowed_real_height_difference = 0.5f;
    const float maximum_allowed_polar_distance = 50.0f;
    
    assert((m_sad_factor + m_height_factor + m_polar_dist_factor + m_polar_sad_factor + m_dense_tracking_factor) == 1.0f);
    
    const float maximum_real_motion = maximum_pedestrian_speed / video_frame_rate;
    
    const unsigned int number_of_current_stixels = current_stixels_p->size();
    const unsigned int number_of_previous_stixels = previous_stixels_p->size();
        
    mp_polarCalibration->getStoredRectifiedImages(m_polarImg1, m_polarImg2);
    
    mp_polarCalibration->getInverseMaps(m_mapXprev, m_mapYprev, 1);
    mp_polarCalibration->getInverseMaps(m_mapXcurr, m_mapYcurr, 2);
    
    cv::Mat currPolar2LinearX, currPolar2LinearY;
    mp_polarCalibration->getMaps(currPolar2LinearX, currPolar2LinearY, 2);
    
    
    motion_cost_matrix.fill( 0.f );
    pixelwise_sad_matrix.fill( 0.f );
    real_height_differences_matrix.fill( 0.f );
    m_stixelsPolarDistMatrix.fill(0.f);
    m_polarSADMatrix.fill(0.f);
    m_denseTrackingMatrix.fill(0.f);
    motion_cost_assignment_matrix.fill( false );
    
    current_stixel_depths.fill( 0.f );
    current_stixel_real_heights.fill( 0.f );
    
    
    // Fill in the motion cost matrix
//     #pragma omp parallel for schedule(dynamic)
    for( unsigned int s_current = 0; s_current < number_of_current_stixels; ++s_current )
    {
        const Stixel& current_stixel = ( *current_stixels_p )[ s_current ];
//         const cv::Point2d current_polar = get_polar_point(mapXcurr, mapYcurr, current_stixel);
        const cv::Point2d current_polar = get_polar_point(m_mapXcurr, m_mapYcurr, currPolar2LinearX, currPolar2LinearY, current_stixel);
        
        const unsigned int stixel_horizontal_padding = compute_stixel_horizontal_padding( current_stixel );
        
        /// Do NOT add else conditions since it can affect the computation of matrices
        if( current_stixel.x - ( current_stixel.width - 1 ) / 2 - stixel_horizontal_padding >= 0 &&
            current_stixel.x + ( current_stixel.width - 1 ) / 2 + stixel_horizontal_padding < current_image_view.width() /*&&
            current_stixel.type != Stixel::Occluded*/ ) // Horizontal padding for current stixel is suitable
        {
            const float current_stixel_disparity = std::max< float >( MIN_FLOAT_DISPARITY, current_stixel.disparity );
            const float current_stixel_depth = stereo_camera.disparity_to_depth( current_stixel_disparity );
            
            const float current_stixel_real_height = compute_stixel_real_height( current_stixel );
            
            // Store for future reference
            current_stixel_depths( s_current ) = current_stixel_depth;
            current_stixel_real_heights( s_current ) = current_stixel_real_height;
            
            for( unsigned int s_prev = 0; s_prev < number_of_previous_stixels; ++s_prev )
            {
                const Stixel& previous_stixel = ( *previous_stixels_p )[ s_prev ];
//                 const cv::Point2d previous_polar = get_polar_point(mapXprev, mapYprev, previous_stixel);
                const cv::Point2d previous_polar = get_polar_point(m_mapXprev, m_mapYprev, currPolar2LinearX, currPolar2LinearY, previous_stixel);
                
                if( previous_stixel.x - ( previous_stixel.width - 1 ) / 2 - stixel_horizontal_padding >= 0 &&
                    previous_stixel.x + ( previous_stixel.width - 1 ) / 2 + stixel_horizontal_padding < previous_image_view.width())
                {
                    const float previous_stixel_disparity = std::max< float >( MIN_FLOAT_DISPARITY, previous_stixel.disparity );
                    const float previous_stixel_depth = stereo_camera.disparity_to_depth( previous_stixel_disparity );
                    
                    if( fabs( current_stixel_depth - previous_stixel_depth ) < maximum_depth_difference )
                    {
                        const int pixelwise_motion = previous_stixel.x - current_stixel.x; // Motion can be positive or negative
                        
                        const unsigned int maximum_motion_in_pixels_for_current_stixel = compute_maximum_pixelwise_motion_for_stixel( current_stixel );
                        
                        if( pixelwise_motion >= -( int( maximum_motion_in_pixels_for_current_stixel ) ) &&
                            pixelwise_motion <= int( maximum_motion_in_pixels_for_current_stixel ))
                        {
                            float pixelwise_sad;
                            float real_height_difference;
                            float polar_distance;
                            float polar_SAD;
                            float denseTrackingScore;
                            
                            if( current_stixel.type != Stixel::Occluded && previous_stixel.type != Stixel::Occluded )
                            {
                                pixelwise_sad = (m_sad_factor == 0.0f)? 0.0f : compute_pixelwise_sad( current_stixel, previous_stixel, current_image_view, previous_image_view, stixel_horizontal_padding );
                                real_height_difference = (m_height_factor == 0.0f)? 0.0f : fabs( current_stixel_real_height - compute_stixel_real_height( previous_stixel ) );
                                polar_distance = (m_polar_dist_factor == 0.0f)? 0.0f : cv::norm(previous_polar - current_polar);
//                                 polar_SAD = (m_polar_sad_factor == 0.0f)? 0.0f : compute_polar_SAD(current_stixel, previous_stixel, current_image_view, previous_image_view, stixel_horizontal_padding);
                                polar_SAD = (m_polar_sad_factor == 0.0f)? 0.0f : compute_polar_SAD(current_stixel, previous_stixel);
                                denseTrackingScore = (m_dense_tracking_factor == 0.0f)? 0.0f : compute_dense_tracking_score(current_stixel, previous_stixel);
                            }
                            else
                            {
                                pixelwise_sad = maximum_pixel_value;
                                real_height_difference = maximum_allowed_real_height_difference;
                                polar_distance = maximum_allowed_polar_distance;
                                polar_SAD = maximum_pixel_value;
                                denseTrackingScore = maximum_pixel_value;
                            }
                            
                            pixelwise_sad_matrix( pixelwise_motion + maximum_possible_motion_in_pixels, s_current ) = pixelwise_sad;
                            real_height_differences_matrix( pixelwise_motion + maximum_possible_motion_in_pixels, s_current ) =
                                    std::min( 1.0f, real_height_difference / maximum_allowed_real_height_difference );
                            
                            m_stixelsPolarDistMatrix( pixelwise_motion + maximum_possible_motion_in_pixels, s_current ) = 1.0f -
                                    std::min( 1.0f, polar_distance / maximum_allowed_polar_distance );
                                    
                            m_polarSADMatrix( pixelwise_motion + maximum_possible_motion_in_pixels, s_current ) = maximum_pixel_value - polar_SAD;
                            
                            m_denseTrackingMatrix( pixelwise_motion + maximum_possible_motion_in_pixels, s_current ) = denseTrackingScore;
                            
                            motion_cost_assignment_matrix( pixelwise_motion + maximum_possible_motion_in_pixels, s_current ) = true;
                            
//                             if (polar_distance > 5.0)
//                                 motion_cost_assignment_matrix( pixelwise_motion + maximum_possible_motion_in_pixels, s_current ) = false;
                        }
                    }
                }
                
            } // End of for( s_prev )
        }
        
    } // End of for( s_current )
   
    /// Rescale the real height difference matrix elemants so that it will have the same range with pixelwise_sad
    const float maximum_real_height_difference = real_height_differences_matrix.maxCoeff();
    //    real_height_differences_matrix = real_height_differences_matrix * ( float ( maximum_pixel_value ) / maximum_real_height_difference );
//     real_height_differences_matrix = real_height_differences_matrix * maximum_pixel_value;
    real_height_differences_matrix = real_height_differences_matrix * (maximum_pixel_value / maximum_real_height_difference);
    
    const float maximum_dense_tracking_value = m_denseTrackingMatrix.maxCoeff();
    m_denseTrackingMatrix = m_denseTrackingMatrix * (maximum_pixel_value / maximum_dense_tracking_value);
    for (uint32_t i = 0; i < m_denseTrackingMatrix.rows(); i++) {
        for (uint32_t j = 0; j < m_denseTrackingMatrix.cols(); j++) {
            m_denseTrackingMatrix(i, j) = maximum_pixel_value - m_denseTrackingMatrix(i, j);
        }
    }
    
    const float maximum_polar_dist_value = m_stixelsPolarDistMatrix.maxCoeff();
    m_stixelsPolarDistMatrix = m_stixelsPolarDistMatrix * (maximum_pixel_value / maximum_polar_dist_value);
    
    /// Fill in the motion cost matrix
//     motion_cost_matrix = alpha * pixelwise_sad_matrix + ( 1 - alpha ) * real_height_differences_matrix; // [0, 255]
    motion_cost_matrix = m_sad_factor * pixelwise_sad_matrix + 
                         m_height_factor * real_height_differences_matrix +
                         m_polar_dist_factor * m_stixelsPolarDistMatrix + 
                         m_polar_sad_factor * m_polarSADMatrix +
                         m_dense_tracking_factor * m_denseTrackingMatrix;
                         
    /*{
        cv::Mat scores(m_denseTrackingMatrix.rows(), m_denseTrackingMatrix.cols(), CV_8UC1);
        for (uint32_t i = 0; i < m_denseTrackingMatrix.rows(); i++) {
            for (uint32_t j = 0; j < m_denseTrackingMatrix.cols(); j++) {
                scores.at<uchar>(i, j) = m_denseTrackingMatrix(i, j);
            }
        }
        cv::imshow("scoresDT", scores);
    }
    {
        cv::Mat scores(pixelwise_sad_matrix.rows(), pixelwise_sad_matrix.cols(), CV_8UC1);
        for (uint32_t i = 0; i < pixelwise_sad_matrix.rows(); i++) {
            for (uint32_t j = 0; j < pixelwise_sad_matrix.cols(); j++) {
                scores.at<uchar>(i, j) = pixelwise_sad_matrix(i, j);
            }
        }
        cv::imshow("pixelwise_sad_matrix", scores);
    }
    {
        cv::Mat scores(real_height_differences_matrix.rows(), real_height_differences_matrix.cols(), CV_8UC1);
        for (uint32_t i = 0; i < real_height_differences_matrix.rows(); i++) {
            for (uint32_t j = 0; j < real_height_differences_matrix.cols(); j++) {
                scores.at<uchar>(i, j) = real_height_differences_matrix(i, j);
            }
        }
        cv::imshow("real_height_differences_matrix", scores);
    }
    {
        cv::Mat scores(m_stixelsPolarDistMatrix.rows(), m_stixelsPolarDistMatrix.cols(), CV_8UC1);
        for (uint32_t i = 0; i < m_stixelsPolarDistMatrix.rows(); i++) {
            for (uint32_t j = 0; j < m_stixelsPolarDistMatrix.cols(); j++) {
                scores.at<uchar>(i, j) = m_stixelsPolarDistMatrix(i, j);
            }
        }
        cv::imshow("m_stixelsPolarDistMatrix", scores);
    }
    {
        cv::Mat scores(m_polarSADMatrix.rows(), m_polarSADMatrix.cols(), CV_8UC1);
        for (uint32_t i = 0; i < m_polarSADMatrix.rows(); i++) {
            for (uint32_t j = 0; j < m_polarSADMatrix.cols(); j++) {
                scores.at<uchar>(i, j) = m_polarSADMatrix(i, j);
            }
        }
        cv::imshow("m_polarSADMatrix", scores);
    }
    {
        cv::Mat scores(motion_cost_matrix.rows(), motion_cost_matrix.cols(), CV_8UC1);
        for (uint32_t i = 0; i < motion_cost_matrix.rows(); i++) {
            for (uint32_t j = 0; j < motion_cost_matrix.cols(); j++) {
                scores.at<uchar>(i, j) = motion_cost_matrix(i, j);
            }
        }
        cv::imshow("scoresMotionCost", scores);
    }*/
                         
    const float maximum_cost_matrix_element = motion_cost_matrix.maxCoeff(); // Minimum is 0 by definition
    
    /// Fill in disappearing stixel entries specially
    //    insertion_cost_dp = maximum_cost_matrix_element * 0.75;
    insertion_cost_dp = maximum_pixel_value * 0.6;
    deletion_cost_dp = insertion_cost_dp; // insertion_cost_dp is not used for the moment !!
    
    {
//         const unsigned int number_of_cols = motion_cost_matrix.cols();
//         const unsigned int largest_row_index = motion_cost_matrix.rows() - 1;
        
    for( unsigned int j = 0, number_of_cols = motion_cost_matrix.cols(), largest_row_index = motion_cost_matrix.rows() - 1; j < number_of_cols; ++j )
//         #pragma omp parallel for schedule(dynamic)
        for( unsigned int j = 0; j < number_of_cols; ++j )
        {
            motion_cost_matrix( largest_row_index, j ) = deletion_cost_dp;
            motion_cost_assignment_matrix( largest_row_index, j ) = true;
            
        } // End of for(j)
    }
    
    {
//         const unsigned int number_of_rows = motion_cost_matrix.rows();
//         const unsigned int number_of_cols = motion_cost_matrix.cols();
                
        for( unsigned int i = 0, number_of_rows = motion_cost_matrix.rows(); i < number_of_rows; ++i )
//         #pragma omp parallel for schedule(dynamic)
//         for( unsigned int i = 0; i < number_of_rows; ++i )
        {
            for( unsigned int j = 0, number_of_cols = motion_cost_matrix.cols(); j < number_of_cols; ++j )
            {
                if( motion_cost_assignment_matrix( i, j ) == false )
                {
                    motion_cost_matrix( i, j ) = 1.2 * maximum_cost_matrix_element;
                    // motion_cost_assignment_matrix(i,j) should NOT be set to true for the entries which are "forced".
                }
            }
        }    
    }
    
    /**
     * 
     * Lines below are intended for DEBUG & VISUALIZATION purposes
     *
     **/
    
    //    fill_in_visualization_motion_cost_matrix();
    cout << "Time for " << __FUNCTION__ << ":" << __LINE__ << " " << omp_get_wtime() - startWallTime << endl;
    
    return;
}

void StixelsTracker::compute_maximum_pixelwise_motion_for_stixel_lut( ) 
{
    m_maximal_pixelwise_motion_by_disp = Eigen::MatrixXi::Zero(MAX_DISPARITY, 1);
    for (uint32_t disp = 0; disp < MAX_DISPARITY; disp++) {
        float disparity = std::max< float >( MIN_FLOAT_DISPARITY, disp );
        float depth = stereo_camera.disparity_to_depth( disparity );

        Eigen::Vector3f point3d1( -maximum_displacement_between_frames / 2, 0, depth );
        Eigen::Vector3f point3d2( maximum_displacement_between_frames / 2, 0, depth );
        
        const MetricCamera& left_camera = stereo_camera.get_left_camera();
        
        Eigen::Vector2f point2d1 = left_camera.project_3d_point( point3d1 );
        Eigen::Vector2f point2d2 = left_camera.project_3d_point( point3d2 );
        
        m_maximal_pixelwise_motion_by_disp(disp, 0) = static_cast<unsigned int>( fabs( point2d2[ 0 ] - point2d1[ 0 ] ) );
    }
}

inline
uint32_t StixelsTracker::compute_maximum_pixelwise_motion_for_stixel( const Stixel& stixel )
{
    return m_maximal_pixelwise_motion_by_disp(stixel.disparity, 0);
}

void StixelsTracker::estimate_stixel_direction()
{
    for (uint32_t i = 0; i < m_tracker.size(); i++) {
        Stixel3d & stixel = m_tracker[i][m_tracker[i].size() - 1];
        
        stixel.direction = cv::Vec2d(0.0f, 0.0f);
        uint32_t numVectors = 0;
        for (uint32_t y = stixel.top_y; y <= stixel.bottom_y; y++) {
            const cv::Point2i currPoint(stixel.x, y);
            const cv::Point2i prevPoint = mp_denseTracker->getPrevPoint(currPoint);
            
            if (prevPoint != cv::Point2i(-1, -1)) {
                stixel.direction += cv::Vec2d(prevPoint.x - currPoint.x, prevPoint.y - currPoint.y);
                numVectors++;
            }
        }
        
        stixel.direction /= (double)numVectors;
//         stixel.direction /= cv::norm(stixel.direction);
        cout << stixel.direction << endl;
    }
}

void StixelsTracker::compute_static_stixels()
{
    
    cv::Mat mapXprev, mapYprev;
    mp_polarCalibration->getInverseMaps(mapXprev, mapYprev, 1);
    cv::Mat currPolar2LinearX, currPolar2LinearY;
    mp_polarCalibration->getMaps(currPolar2LinearX, currPolar2LinearY, 2);
    
    // Rectified difference is obtained
    cv::Mat diffRect;
    {
        cv::Mat polar1, polar2, diffPolar;
        mp_polarCalibration->getStoredRectifiedImages(polar1, polar2);
        cv::Mat polar1gray(polar1.size(), CV_8UC1);
        cv::Mat polar2gray(polar1.size(), CV_8UC1);
        cv::cvtColor(polar1, polar1gray, CV_BGR2GRAY);
        cv::cvtColor(polar2, polar2gray, CV_BGR2GRAY);
        cv::absdiff(polar1gray, polar2gray, diffPolar);
        
        cv::Mat inverseX, inverseY;
        mp_polarCalibration->getInverseMaps(inverseX, inverseY, 1);
        cv::remap(diffPolar, diffRect, inverseX, inverseY, cv::INTER_CUBIC, cv::BORDER_CONSTANT);
    }
//     cv::threshold(diffRect, diffRect, 30, 255, cv::THRESH_BINARY);
    
    cv::Mat diffRectColor(diffRect.size(), CV_8UC3);
    cv::cvtColor(diffRect, diffRectColor, CV_GRAY2BGR);
    cv::Mat diffRectColorBig;
    cv::resize(diffRectColor, diffRectColorBig, cv::Size(1920, 1200));
    
    for (stixels_t::iterator it = current_stixels_p->begin(), it2 = previous_stixels_p->begin(); 
                    it != current_stixels_p->end(); it++, it2++) {
        
        double totalDiffs = 0.0f;
        {
            for (uint32_t j = min(it->bottom_y, it2->bottom_y); j <= max(it->bottom_y, it2->bottom_y); j++) {
                if (diffRect.at<uint8_t>(j, it->x) == 255)
                    totalDiffs++;
            }
            totalDiffs /= fabs(it->bottom_y - it->bottom_y) + 1;
        }
        
        //         cv::line(diffRectColor, cv::Point2d(it->x, it->bottom_y), cv::Point2d(it2->x, it2->bottom_y), cv::Scalar(255 - 255 * totalDiffs, 0, 255 * totalDiffs));
        //         cv::circle(diffRectColor, cv::Point2d(it->x, it->bottom_y), 1, cv::Scalar(255 - 255 * totalDiffs, 0, 255 * totalDiffs), -1);
        //         cv::circle(diffRectColor, cv::Point2d(it2->x, it2->bottom_y), 1, cv::Scalar(255 - 255 * totalDiffs, 0, 255 * totalDiffs), -1);
    }
        
//     for (stixels_t::iterator it = current_stixels_p->begin(), it2 = previous_stixels_p->begin(); 
//              it != current_stixels_p->end(); it++, it2++) {
// //         const cv::Point2d currPoint(it->x, it->bottom_y);
//         const cv::Point2d lastPoint(it2->x, it2->bottom_y);
//         const cv::Point2d & lastPointNow = get_polar_point(mapXprev, mapYprev, currPolar2LinearX, currPolar2LinearY, *it2);
//         cv::Point2d currPoint(-1, -1);
//         if (lastPointNow != currPoint)
//             currPoint = cv::Point2d(current_stixels_p->at(lastPointNow.x).x, current_stixels_p->at(lastPointNow.x).bottom_y);
    
    stixels_motion_t corresp = stixels_motion;

    for (uint32_t prevPos = 0; prevPos < previous_stixels_p->size(); prevPos++) {
        
        uint32_t currPos = 0;
        for (; currPos < corresp.size(); currPos++)
            if (corresp[currPos] == prevPos)
                break;
            
        cv::Point2d currPoint(-1, -1);
        if (currPos != corresp.size())
            currPoint = cv::Point2d(current_stixels_p->at(currPos).x, current_stixels_p->at(currPos).bottom_y);
        
        const cv::Point2d lastPoint(previous_stixels_p->at(prevPos).x, previous_stixels_p->at(prevPos).bottom_y);
        const cv::Point2d & lastPointNow = get_polar_point(mapXprev, mapYprev, currPolar2LinearX, currPolar2LinearY, previous_stixels_p->at(prevPos));

        cv::circle(diffRectColor, currPoint, 1, cv::Scalar(0, 0, 255), -1);
        cv::circle(diffRectColor, lastPointNow, 1, cv::Scalar(255, 0, 0), -1);
        cv::circle(diffRectColor, lastPoint, 1, cv::Scalar(0, 255, 0), -1);
        
        const float factorX = (float)diffRectColorBig.cols / (float)diffRectColor.cols;
        const float factorY = (float)diffRectColorBig.rows / (float)diffRectColor.rows;
        const cv::Point2d currPointBig(currPoint.x * factorX, currPoint.y * factorY);
        const cv::Point2d lastPointBig(lastPoint.x * factorX, lastPoint.y * factorY);
        const cv::Point2d lastPointNowBig(lastPointNow.x * factorX, lastPointNow.y * factorY);
        
        cv::Scalar color(rand() & 0xFF, rand() & 0xFF, rand() & 0xFF);
        if ((currPoint != cv::Point2d(-1, -1)) && (lastPointNow != cv::Point2d(-1, -1)))
            cv::line(diffRectColorBig, currPointBig, lastPointNowBig, color);
        if ((lastPointNow != cv::Point2d(-1, -1)) && (lastPoint != cv::Point2d(-1, -1)))
            cv::line(diffRectColorBig, lastPointNowBig, lastPointBig, color);
        
        cv::circle(diffRectColorBig, currPointBig, 1, cv::Scalar(0, 0, 255), -1);
        cv::circle(diffRectColorBig, lastPointNowBig, 1, cv::Scalar(255, 0, 0), -1);
        cv::circle(diffRectColorBig, lastPointBig, 1, cv::Scalar(0, 255, 0), -1);
    }
    
    cv::imshow("Thresh1", diffRectColor);
    cv::imshow("polarTrack", diffRectColorBig);
}

float StixelsTracker::compute_dense_tracking_score(const Stixel& currStixel, const Stixel& prevStixel)
{

    float matched = 0.0f, unmatched = 0.0f;
    for (uint32_t y = currStixel.top_y; y <= currStixel.bottom_y; y++) {
        const cv::Point2i currPoint(currStixel.x, y);
        const cv::Point2i prevPoint = mp_denseTracker->getPrevPoint(currPoint);
        
        if (prevPoint != cv::Point2i(-1, -1)) {
            if (prevPoint.x == prevStixel.x)
                matched += 1.0f;
            else
                unmatched += 1.0f;
        }
    }
    
//     if ((matched + unmatched) == 0)
//         return 0;
//     else
//         return 255 * matched / (matched + unmatched);
    return matched;
}

float StixelsTracker::compute_polar_SAD(const Stixel& stixel1, const Stixel& stixel2)
{
    
    cv::Point2d point1(stixel1.x, 0.0f);
    cv::Point2d point2(stixel2.x, 0.0f);
    
    const double height1 = stixel1.bottom_y - stixel1.top_y; 
    const double height2 = stixel2.bottom_y - stixel2.top_y;
    const double height = max(height1, height2);
    
    const double factor1 = height1 / height;
    const double factor2 = height2 / height;
    
    cv::Mat polarImg1, polarImg2;
    mp_polarCalibration->getStoredRectifiedImages(polarImg1, polarImg2);
    
    cv::Mat mapXprev, mapYprev, mapXcurr, mapYcurr;
    mp_polarCalibration->getInverseMaps(mapXprev, mapYprev, 1);
    mp_polarCalibration->getInverseMaps(mapXcurr, mapYcurr, 2);
    
    float sad = 0.0;
    double validPoints = 0.0f;

    for (uint32_t i = 0; i <= height; i++) {
        const cv::Point2d pos1 = cv::Point2d(stixel1.x, stixel1.top_y + factor1 * i);
        const cv::Point2d pos2 = cv::Point2d(stixel2.x, stixel2.top_y + factor2 * i);
    
        cv::Point2d p1, p2;
        p1 = get_polar_point(mapXprev, mapYprev, pos1);
        p2 = get_polar_point(mapXcurr, mapYcurr, pos2);
        
        if ((p1 == cv::Point2d(-1, -1)) || (p2 == cv::Point2d(-1, -1)))
            continue;
        
        validPoints += 1.0f;
            
        const cv::Vec3b & px1 = polarImg2.at<cv::Vec3b>(p1.y, p1.x);
        const cv::Vec3b & px2 = polarImg1.at<cv::Vec3b>(p2.y, p2.x);
        
        const cv::Vec3b diffPx = px1 - px2;
        sad += fabs(cv::sum(diffPx)[0]);
    }
    
    return sad / validPoints / polarImg1.channels();
}

float StixelsTracker::compute_polar_SAD(const Stixel& stixel1, const Stixel& stixel2,
                                        const input_image_const_view_t& image_view1, const input_image_const_view_t& image_view2,
                                        const unsigned int stixel_horizontal_padding)
{
    const unsigned int stixel_representation_width = stixel1.width + 2 * stixel_horizontal_padding;
    
    const unsigned int number_of_channels = image_view1.num_channels();
    
    stixel_representation_t stixel_representation1;
    stixel_representation_t stixel_representation2;
    
    compute_stixel_representation_polar( stixel1, image_view1, stixel_representation1, stixel_horizontal_padding, m_mapXcurr, m_mapYcurr, m_polarImg2 );    
    compute_stixel_representation_polar( stixel2, image_view2, stixel_representation2, stixel_horizontal_padding, m_mapXprev, m_mapYprev, m_polarImg1 );
    
    float pixelwise_sad = 0;
    
    for( unsigned int c = 0; c < number_of_channels; ++c )
    {
        const Eigen::MatrixXf& current_stixel_representation_channel = stixel_representation1[ c ];
        const Eigen::MatrixXf& previous_stixel_representation_channel = stixel_representation2[ c ];
        
        for( unsigned int y = 0; y < stixel_representation_height; ++y )
        {
            for( unsigned int x = 0; x < stixel_representation_width; ++x )
            {
                pixelwise_sad += fabs( current_stixel_representation_channel( y, x ) - previous_stixel_representation_channel( y, x ) );
                
            } // End of for( x )
            
        } // End of for( y )
        
    } // End of for( c )
    
    pixelwise_sad = pixelwise_sad / number_of_channels;
    pixelwise_sad = pixelwise_sad / ( stixel_representation_height * stixel_representation_width );
    
    stixel_representation1.clear();
    stixel_representation2.clear();
    
    return pixelwise_sad;
}

void StixelsTracker::compute_stixel_representation_polar( const Stixel &stixel, const input_image_const_view_t& image_view_hosting_the_stixel,
                                                          stixel_representation_t &stixel_representation, const unsigned int stixel_horizontal_padding,
                                                          const cv::Mat & mapX, const cv::Mat & mapY, const cv::Mat & polarImg )
{
    const unsigned int stixel_representation_width = stixel.width + 2 * stixel_horizontal_padding;    
    
    const int stixel_height = abs( stixel.top_y - stixel.bottom_y );
    const int stixel_effective_part_height = stixel_height;
    
    const float reduction_ratio = float( stixel_representation_height ) / float( stixel_effective_part_height );
    
    // Image boundary conditions are NOT checked for speed efficiency !
    if( (stixel.width % 2) != 1 ) {
        printf("stixel.width == %i\n", stixel.width);
        throw std::invalid_argument( "DummyStixelMotionEstimator::compute_stixel_representation() -- The width of stixel should be an odd number !" );
    }
    
    const int32_t minX = stixel.x - ( stixel.width - 1 ) / 2 - stixel_horizontal_padding;
    const int32_t maxX = stixel.x + ( stixel.width - 1 ) / 2 + stixel_horizontal_padding;
    
//     if( minX < 0 || maxX >= image_view_hosting_the_stixel.width() ) {
    if( stixel.x - ( stixel.width - 1 ) / 2 - stixel_horizontal_padding < 0 ||
        stixel.x + ( stixel.width - 1 ) / 2 + stixel_horizontal_padding >= image_view_hosting_the_stixel.width() )
    {
        
        throw std::invalid_argument( "DummyStixelMotionEstimator::compute_stixel_representation() -- The stixel representation should obey the image boundaries !" );
    }    
    
    const unsigned int number_of_channels = image_view_hosting_the_stixel.num_channels();
    
    stixel_representation.clear();
    stixel_representation.resize( number_of_channels );   
    
//     cv::Mat dbgImg = cv::Mat::zeros(stixel_representation_height, stixel_representation_width, CV_8UC3);
    
    for( unsigned int c = 0; c < number_of_channels; ++c ) {
        stixel_representation[ c ].resize( stixel_representation_height, stixel_representation_width );
        
    } // End of for( c )
    
    for( unsigned int y = 0; y < stixel_representation_height; ++y ) {
        const float projected_y = float( y ) / reduction_ratio;
        
        const float projected_upper_y = std::ceil( projected_y );
        const float projected_lower_y = std::floor( projected_y );
        
        // The coefficients are in reverse order (sum of coefficients is 1)
        float coefficient_lower_y = projected_upper_y - projected_y;
        float coefficient_upper_y = projected_y - projected_lower_y;
        
        // If the projected pixel falls just on top of an integer coordinate
        if( coefficient_lower_y + coefficient_upper_y < 0.05 ) {
            coefficient_lower_y = 0.5;
            coefficient_upper_y = 0.5;
        }
                
        for( unsigned int x = 0; x < stixel_representation_width; ++x ) {
            cv::Point2d polarLower = get_polar_point(mapX, mapY, cv::Point2d(x, projected_lower_y));
            cv::Point2d polarUpper = get_polar_point(mapX, mapY, cv::Point2d(x, projected_upper_y));
                        
            if ((polarLower != cv::Point2d(-1, -1)) && (polarUpper != cv::Point2d(-1, -1))) {
                    
                const cv::Vec3b & pxLower = polarImg.at<cv::Vec3b>(polarLower.y, polarLower.x);
                const cv::Vec3b & pxUpper = polarImg.at<cv::Vec3b>(polarLower.y, polarLower.x);
                
                for( unsigned int c = 0; c < number_of_channels; ++c ) {
                    ( stixel_representation[ c ] )( y, x ) = coefficient_lower_y * pxLower[ c ] + coefficient_upper_y * pxUpper[ c ];
                } // End of for( c )
            }
            
        } // End of for( x )
        
    } // End of for( y )
    
//     for( unsigned int y = stixel.top_y, representationY = 0; y < stixel.bottom_y; ++y, representationY++) {
// 
//         
//         
//         float projectedY = reduction_ratio * representationY; 
//         
//         for( unsigned int x = minX, representationX = 0; x <= maxX; ++x, representationX++ ) {
//             
//         }
//     }

//     for( unsigned int y = stixel.top_y, representationY = 0; y < stixel.bottom_y; ++y, representationY++ ) {
//         
//         const float reduced_Y = float( representationY ) / reduction_ratio;
// 
//         for( unsigned int x = minX, representationX = 0; x <= maxX; ++x, representationX++ ) {
//         
//             cv::Point2d p = get_polar_point(mapX, mapY, cv::Point2d(x, y));
//             
//             if (p != cv::Point2d(-1, -1)) {
//         
//                 const float projected_x = p.x;
//                 const float projected_y = p.y;
//                 
//                 const float projected_upper_y = std::ceil( projected_y );
//                 const float projected_lower_y = std::floor( projected_y );
//             
//                 // The coefficients are in reverse order (sum of coefficients is 1)
//                 float coefficient_lower_y = projected_upper_y - projected_y;
//                 float coefficient_upper_y = projected_y - projected_lower_y;
//                 
//                 // If the projected pixel falls just on top of an integer coordinate
//                 if( coefficient_lower_y + coefficient_upper_y < 0.05 ) {
//                     coefficient_lower_y = 0.5;
//                     coefficient_upper_y = 0.5;
//                 }
// 
//                 const cv::Vec3b & pxLower = m_polarImg2.at<cv::Vec3b>(projected_lower_y, projected_x);
//                 const cv::Vec3b & pxUpper = m_polarImg1.at<cv::Vec3b>(projected_upper_y, projected_x);
//                 
//                 for( unsigned int c = 0; c < number_of_channels; ++c ) {
//                     cout << "c " << c << endl;
//                     cout << "number_of_channels " << number_of_channels << endl;
//                     cout << "reduced_Y " << reduced_Y << endl;
//                     cout << "representationX " << representationX << endl;
//                     cout << "pxLower " << pxLower << endl;
//                     cout << "pxUpper " << pxUpper << endl;
//                     cout << "coefficient_lower_y " << representationX << endl;
//                     cout << "coefficient_upper_y " << representationX << endl;
//                     cout << "( stixel_representation[ c ] )( reduced_Y, representationX ) " << ( stixel_representation[ c ] )( (unsigned int)reduced_Y, representationX ) << endl;
//                     ( stixel_representation[ c ] )( (unsigned int)reduced_Y, representationX ) = 0; //coefficient_lower_y * pxLower[c] + coefficient_upper_y * pxUpper[c];
// //                     dbgImg.at<cv::Vec3b>(reduced_Y, representationX)[c] = coefficient_lower_y * pxLower[c] + coefficient_upper_y * pxUpper[c];
//                 } // End of for( c )
//             }
//         } // End of for( x )
//     } // End of for( y )
    
//     cv::imshow("dbgImg", dbgImg);
//     cv::waitKey(0);
    
    return;
}

void StixelsTracker::draw_polar_SAD(cv::Mat& img, const Stixel& stixel1, const Stixel& stixel2)
{
    cv::Point2d point1(stixel1.x, 0.0f);
    cv::Point2d point2(stixel2.x, 0.0f);
    
    const double height1 = stixel1.bottom_y - stixel1.top_y; 
    const double height2 = stixel2.bottom_y - stixel2.top_y;
    const double height = max(height1, height2);
    
    const double factor1 = height1 / height;
    const double factor2 = height2 / height;
    
    cv::Mat polarImg1, polarImg2;
    mp_polarCalibration->getStoredRectifiedImages(polarImg1, polarImg2);
    
    cv::Mat mapXprev, mapYprev, mapXcurr, mapYcurr;
    mp_polarCalibration->getInverseMaps(mapXprev, mapYprev, 1);
    mp_polarCalibration->getInverseMaps(mapXcurr, mapYcurr, 2);
    
    for (uint32_t i = 0; i <= height; i++) {
        const cv::Point2d pos1 = cv::Point2d(stixel1.x, stixel1.top_y + factor1 * i);
        const cv::Point2d pos2 = cv::Point2d(stixel2.x, stixel2.top_y + factor2 * i);
        
        cv::Point2d p1, p2;
        p1 = get_polar_point(mapXprev, mapYprev, pos1);
        p2 = get_polar_point(mapXcurr, mapYcurr, pos2);
        
        if ((p1 == cv::Point2d(-1, -1)) || (p2 == cv::Point2d(-1, -1))) {
            img.at<cv::Vec3b>(pos1.y, pos1.x)= cv::Vec3b::all(0);
        } else {
            const cv::Vec3b & px1 = polarImg1.at<cv::Vec3b>(p1.y, p1.x);
            const cv::Vec3b & px2 = polarImg2.at<cv::Vec3b>(p2.y, p2.x);
            
            const cv::Vec3b diffPx = px1 - px2;
            double sad = fabs(cv::sum(diffPx)[0]) / 3.0;
            
            img.at<cv::Vec3b>(pos1.y, pos1.x)= cv::Vec3b::all(sad);
        }
    }
}

void StixelsTracker::computeMotionWithGraphs()
{
    lemon::SmartGraph graph;
    lemon::SmartGraph::EdgeMap <float> costs(graph);
    lemon::SmartGraph::NodeMap <uint32_t> nodeIdx(graph);
    graph.reserveNode(current_stixels_p->size() + previous_stixels_p->size());
    graph.reserveEdge(current_stixels_p->size() * previous_stixels_p->size());
    
    BOOST_FOREACH (const Stixel & stixel, *previous_stixels_p)
        nodeIdx[graph.addNode()] = stixel.x;
    BOOST_FOREACH (const Stixel & stixel, *current_stixels_p)
        nodeIdx[graph.addNode()] = stixel.x;
    
    const float maxCost = motion_cost_matrix.maxCoeff();
    for (uint32_t prevIdx = 0; prevIdx < previous_stixels_p->size(); prevIdx++) {
        const Stixel & prevStixel = previous_stixels_p->at(prevIdx);
        for (uint32_t currIdx = 0; currIdx < current_stixels_p->size(); currIdx++) {
            const Stixel & currStixel = current_stixels_p->at(currIdx);
            
            const int32_t pixelwise_motion = prevStixel.x - currStixel.x;
            const uint32_t & maximum_motion_in_pixels_for_current_stixel = compute_maximum_pixelwise_motion_for_stixel( currStixel );
            
            const int32_t pixelwise_motionY = fabs(prevStixel.bottom_y - currStixel.bottom_y);
            
            const uint32_t rowIndex = pixelwise_motion + maximum_possible_motion_in_pixels;
            
            if( pixelwise_motion >= -( int( maximum_motion_in_pixels_for_current_stixel ) ) &&
                pixelwise_motion <= int( maximum_motion_in_pixels_for_current_stixel ) &&
                pixelwise_motionY <= int( maximum_motion_in_pixels_for_current_stixel ) &&
                (motion_cost_assignment_matrix(rowIndex, currIdx))) {
                    
                const float & polarDist = m_stixelsPolarDistMatrix(rowIndex, currIdx);
                
                if (polarDist > 1.0f) {
                    const float & cost = maxCost - motion_cost_matrix(rowIndex, currIdx);
                    
                    const lemon::SmartGraph::Edge & e = graph.addEdge(graph.nodeFromId(prevIdx), graph.nodeFromId(currIdx + previous_stixels_p->size()));
                    costs[e] = cost;
                }
            }
        }
    }
    
    lemon::MaxWeightedMatching< lemon::SmartGraph, lemon::SmartGraph::EdgeMap <float> > graphMatcher(graph, costs);
    
    graphMatcher.run();
    
    const lemon::SmartGraph::NodeMap<lemon::SmartGraph::Arc> & matchingMap = graphMatcher.matchingMap();
    
    // FIXME
    BOOST_FOREACH(int32_t &i, stixels_motion)
        i = -1;
    for (uint32_t i = 0; i < previous_stixels_p->size(); i++) {
        if (graphMatcher.mate(graph.nodeFromId(i)) != lemon::INVALID) {
            lemon::SmartGraph::Arc arc = matchingMap[graph.nodeFromId(i)];
            stixels_motion[graph.id(graph.target(arc)) - previous_stixels_p->size()] = graph.id(graph.source(arc));
        }
    }
    
    
}

void StixelsTracker::updateTracker()
{
    const stixels_t * currStixels = current_stixels_p;
    stixels_motion_t corresp = stixels_motion;
    
    if (m_tracker.size() == 0) {
        stixels3d_t newStixels3d;
        newStixels3d.reserve(currStixels->size());
        m_tracker.resize(currStixels->size());
        for (uint32_t i = 0; i < currStixels->size(); i++) {
            Stixel3d currStixel3d(currStixels->at(i));
            currStixel3d.update3dcoords(stereo_camera);
            currStixel3d.isStatic = false;
            
            m_tracker[i].push_back(currStixel3d);
            
            currStixel3d.valid_forward_delta_x = false;
            newStixels3d.push_back(currStixel3d);
        }
        m_stixelsHistoric.push_front(newStixels3d);
        return;
    }
    
    if (m_stixelsHistoric.size() > MAX_ITERATIONS_STORED) {
        m_stixelsHistoric.pop_back();
    }
    
    t_tracker tmpTracker(m_tracker.size());
    copy(m_tracker.begin(), m_tracker.end(), tmpTracker.begin());
    m_tracker.clear();
    m_tracker.resize(currStixels->size());
    
    stixels3d_t & lastStixels3d = m_stixelsHistoric[0];
    stixels3d_t newStixels3d;
    newStixels3d.reserve(currStixels->size());
    
    for (uint32_t i = 0; i < currStixels->size(); i++) {
        if (corresp[i] >= 0) {
            m_tracker[i] = tmpTracker[corresp[i]];
        }
        Stixel3d currStixel3d(currStixels->at(i));
        currStixel3d.update3dcoords(stereo_camera);
        
//         currStixel3d.isStatic = false;
//         if ((corresp[i] >= 0) && (compute_polar_SAD(currStixels->at(i), previous_stixels_p->at(corresp[i])) < m_minPolarSADForBeingStatic))
//             currStixel3d.isStatic = true;

        currStixel3d.valid_backward_delta_x = false;
        currStixel3d.valid_forward_delta_x = false;
        if (corresp[i] >= 0) {
            lastStixels3d[corresp[i]].forward_delta_x = i;
            lastStixels3d[corresp[i]].valid_forward_delta_x = true;
            
            currStixel3d.backward_delta_x = corresp[i];
            currStixel3d.valid_backward_delta_x = true;
        }
        
        m_tracker[i].push_back(currStixel3d);
        newStixels3d.push_back(currStixel3d);
    }
    m_stixelsHistoric.push_front(newStixels3d);
}

void StixelsTracker::getClusters()
{
    pcl::PointCloud<pcl::PointXYZL>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZL>);
    cloud->reserve(current_stixels_p->size());
    
    for (uint32_t i = 0; i < m_tracker.size(); i++) {
        pcl::PointXYZL pointPCL;
        if (stixels_motion[i] >= 0) {
            const cv::Point3d & point = m_tracker[i][m_tracker[i].size() - 1].bottom3d;
            pointPCL.x = point.x;
            pointPCL.y = 0.0f; //point.y;
            pointPCL.z = point.z;
            pointPCL.label = 1;
        }
        cloud->push_back(pointPCL);
    }
    
    // Creating the KdTree object for the search method of the extraction
    pcl::search::KdTree<pcl::PointXYZL>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZL>);
    tree->setInputCloud (cloud);
    
    std::vector<pcl::PointIndices> cluster_indices;
    pcl::EuclideanClusterExtraction<pcl::PointXYZL> ec;
    ec.setClusterTolerance (m_minDistBetweenClusters);
    ec.setMinClusterSize (3); 
    ec.setMaxClusterSize (m_tracker.size());
    ec.setSearchMethod (tree);
    ec.setInputCloud (cloud);
    ec.extract (cluster_indices);

    m_clusters.clear();
    m_clusters.resize(m_tracker.size());

    m_objects.clear();
    m_objects.reserve(cluster_indices.size());
    
    uint32_t clusterIdx = 0;
    for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin (); it != cluster_indices.end (); ++it, ++clusterIdx)
    {
        const int32_t & idxBegin = it->indices[0];
        const int32_t & idxEnd = it->indices[it->indices.size() - 1];
        const Stixel3d & stixelBegin = m_tracker[idxBegin][m_tracker[idxBegin].size() - 1];
        const Stixel3d & stixelEnd = m_tracker[idxEnd][m_tracker[idxEnd].size() - 1];
        
        const double clusterWidth = stixelEnd.bottom3d.x - stixelBegin.bottom3d.x; 

        uint32_t trackLenght = 0;
        for (std::vector<int>::const_iterator pit = it->indices.begin(); pit != it->indices.end(); pit++) {
            if ((cloud->at(*pit).label == 0) || (clusterWidth < m_minAllowedObjectWidth)) {
                m_clusters[*pit] = -1;
            } else {
                m_clusters[*pit] = clusterIdx;
                if (m_tracker[*pit].size() > trackLenght)
                    trackLenght = m_tracker[*pit].size();
            }
        }

        if ((clusterWidth > m_minAllowedObjectWidth) && 
            (cloud->at(it->indices[0]).label != 0) && (trackLenght > 2)) {
            
            vector <int> object(it->indices.size());
            copy(it->indices.begin(), it->indices.end(), object.begin());
            m_objects.push_back(object);
        }
    }
}

void StixelsTracker::projectPointInTopView(const cv::Point3d & point3d, const cv::Mat & imgTop, cv::Point2d & point2d)
{
    const double maxDistZ = 20.0;
    const double maxDistX = maxDistZ / 2.0;
    
    // v axis corresponds to Z
    point2d.y = imgTop.rows - ((imgTop.rows - 10) * min(maxDistZ, point3d.z) / maxDistZ);
    
    // u axis corresponds to X
    point2d.x = ((imgTop.cols / 2.0) * min(maxDistX, point3d.x) / maxDistX) + imgTop.cols / 2;
}

void StixelsTracker::drawTracker(cv::Mat& img, cv::Mat & imgTop)
{
    
    if (m_color.size() == 0) {
        m_color.resize(current_stixels_p->size());
        
        uint32_t division = m_color.size() / 3;
        for (uint32_t i = 1; i <= m_color.size(); i++) {
            m_color[i - 1] = cv::Scalar((i * 50) % 256, (i * 100) % 256, (i * 200) % 256);
        }
        
    }
    
    gil2opencv(current_image_view, img);
    imgTop = cv::Mat::zeros(img.rows, img.cols, CV_8UC3);
    
    cv::rectangle(img, cv::Point2d(0, 0), cv::Point2d(img.cols - 1, 20), cv::Scalar::all(0), -1);
    
    stringstream oss;
    oss << "SAD = " << m_sad_factor << ", Height = " << m_height_factor << 
           ", Polar distance = " << m_polar_dist_factor << ", Polar SAD = " << m_polar_sad_factor <<
           ", Dense Tracking = " << m_dense_tracking_factor;
    cv::putText(img, oss.str(), cv::Point2d(5, 15), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar::all(255));
    cv::putText(imgTop, oss.str(), cv::Point2d(2, 7), cv::FONT_HERSHEY_SIMPLEX, 0.25, cv::Scalar::all(255));
    
    if (0) {
        uint32_t clusterIdx = 0;
        for (vector < vector <int> >::iterator it = m_objects.begin(); it != m_objects.end(); it++, clusterIdx++) {
            const cv::Scalar & color =  m_color[clusterIdx];
            
            cv::Point2d corner1(current_stixels_p->at(it->at(0)).x, current_stixels_p->at(it->at(0)).bottom_y);
            cv::Point2d corner2(current_stixels_p->at(it->at(it->size() - 1)).x, current_stixels_p->at(it->at(it->size() - 1)).top_y);
            
            for (vector<int>::iterator it2 = it->begin(); it2 != it->end(); it2++) {
                
                stixels3d_t & track = m_tracker[*it2];
                stixels3d_t::iterator itTrack = track.begin();
                itTrack++;
                for (; itTrack != track.end(); itTrack++) {
                    cv::line(img, itTrack->getBottom2d<cv::Point2d>(), (itTrack - 1)->getBottom2d<cv::Point2d>(), color);
                    
                    cv::Point2d p1Top, p2Top;
                    projectPointInTopView(itTrack->bottom3d, imgTop, p1Top);
                    projectPointInTopView((itTrack - 1)->bottom3d, imgTop, p2Top);
                    cv::line(imgTop, p1Top, p2Top, color);
                }
                
                if (current_stixels_p->at(*it2).bottom_y > corner1.y) 
                    corner1.y = current_stixels_p->at(*it2).bottom_y;
                if (current_stixels_p->at(*it2).top_y < corner2.y) 
                    corner2.y = current_stixels_p->at(*it2).top_y;
            }
            
            cv::rectangle(img, corner1, corner2, color);
        }
    } else if (1) {
        for (vector < stixels3d_t >::iterator it = m_tracker.begin(); it != m_tracker.end(); it++) {
            const cv::Scalar & color =  m_color[it->begin()->x];
//             const cv::Scalar color = /*(it->at(it->size() - 1).isStatic)? cv::Scalar(255, 0, 0) : */cv::Scalar(0, 0, 255);
            for (stixels3d_t::iterator it2 = it->begin() + 1; it2 != it->end(); it2++) {
                const cv::Point2d & p1 = (it2 - 1)->getBottom2d<cv::Point2d>();
                const cv::Point2d & p2 = it2->getBottom2d<cv::Point2d>();
                
//                 draw_polar_SAD(img, *(it2 - 1), *it2);
//                 float sad = compute_polar_SAD(*(it2 - 1), *it2);
//                 const cv::Point2d p2c(p2.x, it2->top_y);
//                 const cv::Point2d p2d(p2.x, it2->top_y - 20);
//                 const cv::Point2d p2e(p2.x, it2->top_y - 40);
// //                 cv::line(img, p2c, p2d, cv::Scalar(0, 0, 255));
//                 cv::line(img, p2d, p2e, cv::Scalar(sad, sad, sad));
                
                cv::line(img, p1, p2, color);
            }
            
//             const cv::Point2d & lastPoint = it->at(it->size() - 1).getBottom2d<cv::Point2d>();
//             cv::circle(img, lastPoint, 3, color, -1);
        }
    } else {
        for (vector < stixels3d_t >::iterator it = m_tracker.begin(); it != m_tracker.end(); it++) {
            const cv::Scalar & color =  m_color[it->begin()->x];

            Stixel3d & stixel = it->at(it->size() - 1);
            const cv::Point2d & p1 = stixel.getBottom2d<cv::Point2d>();
            const cv::Point2d & p2 = p1 + 5 * cv::Point2d(stixel.direction);
            
            cv::circle(img, p1, 1, color, -1);
            cv::line(img, p1, p2, color);
        }
    }
    
    
    cv::rectangle(imgTop, cv::Point2d(0, 0), cv::Point2d(imgTop.cols - 1, imgTop.rows - 1), cv::Scalar::all(255));
}

void StixelsTracker::drawTracker(cv::Mat& img)
{
    cv::rectangle(img, cv::Point2d(0, 0), cv::Point2d(img.cols - 1, 20), cv::Scalar::all(0), -1);
    
    stringstream oss;
    oss << "SAD = " << m_sad_factor << ", Height = " << m_height_factor << 
    ", Polar distance = " << m_polar_dist_factor << ", Polar SAD = " << m_polar_sad_factor <<
    ", Dense Tracking = " << m_dense_tracking_factor;
    cv::putText(img, oss.str(), cv::Point2d(5, 15), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar::all(255));
    
    for (vector < stixels3d_t >::iterator it = m_tracker.begin(); it != m_tracker.end(); it++) {
        const cv::Scalar color = (it->at(it->size() - 1).isStatic)? cv::Scalar(255, 0, 0) : cv::Scalar(0, 0, 255);
        
        const cv::Point2d & lastPointB = it->at(it->size() - 1).getBottom2d<cv::Point2d>();
        const cv::Point2d & lastPointT = it->at(it->size() - 1).getTop2d<cv::Point2d>();
        cv::circle(img, lastPointB, 1, color, -1);
        cv::circle(img, lastPointT, 1, color, -1);
    }
    
    
}

void StixelsTracker::drawDenseTracker(cv::Mat& img)
{
    mp_denseTracker->drawTracks(img);
}