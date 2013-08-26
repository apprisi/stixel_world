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

#ifndef STIXELSAPPLICATION_H
#define STIXELSAPPLICATION_H

#include <string>

#include <boost/program_options.hpp>

#include "utils.h"
#include "polarcalibration.h"

#include "stixel_world_lib.hpp"
#include "video_input/VideoInputFactory.hpp"
#include "video_input/preprocessing/CpuPreprocessor.hpp"
#include "stereo_matching/stixels/AbstractStixelWorldEstimator.hpp"

using namespace std;

namespace stixel_world {

class StixelsApplication
{
public:
    StixelsApplication(const string & optionsFile);
    
    void runStixelsApplication();
private:
    boost::program_options::variables_map parseOptionsFile(const string& optionsFile);
    bool iterate();
    void update();
    void visualize();
    bool rectifyPolar();
    
    boost::shared_ptr<doppia::AbstractVideoInput> mp_video_input;
    boost::shared_ptr<doppia::AbstractStixelWorldEstimator> mp_stixel_world_estimator;
    
//     doppia::AbstractVideoInput::input_image_t m_currentLeft, m_currentRight;
    doppia::AbstractVideoInput::input_image_t m_prevLeftRectified, m_prevRightRectified;
    doppia::AbstractVideoInput::input_image_t m_polarLt0, m_polarRt0, m_polarLt1, m_polarRt1;
    
//     boost::gil::rgb8_view_t m_prevLeftRectified, m_prevRightRectified;
//     stixel_world::input_image_const_view_t m_currentLeftRectified, m_currentRightRectified;
//     stixel_world::input_image_const_view_t m_prevLeftRectified, m_prevRightRectified;
    
//     boost::shared_ptr<stixels_t> mp_currStixels;
    boost::shared_ptr<stixels_t> mp_prevStixels;
//     stixels_t m_currStixels;
    
    boost::program_options::variables_map m_options;
    
    boost::shared_ptr<PolarCalibration> mp_polarCalibration;
};

    
}

#endif // STIXELSAPPLICATION_H
