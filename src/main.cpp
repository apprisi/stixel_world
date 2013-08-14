#include <iostream>
#include <stdio.h>
#include <fstream>

#include <opencv2/opencv.hpp>
#include <boost/filesystem.hpp>
#include <boost/concept_check.hpp>

using namespace std;

#define BASE_PATH "/local/imaged/stixels/bahnhof"
#define IMG1_PATH "seq03-img-left"
#define FILE_STRING1 "image_%08d_0.png"
// #define IMG2_PATH "seq03-img-left"
// #define FILE_STRING2 "image_%08d_0.png"
#define IMG2_PATH "seq03-img-right"
#define FILE_STRING2 "image_%08d_1.png"
#define CALIBRATION_STRING "cam%d.cal"
#define MIN_IDX 138 //120
#define MAX_IDX 999

// #define BASE_PATH "/local/imaged/stixels/castlejpg"
// #define FILE_STRING "castle.%03d.jpg"
// #define MIN_IDX 26
// #define MAX_IDX 27

void getCalibrationMatrix(const boost::filesystem::path &filePath, cv::Mat & cameraMatix, cv::Mat & distCoeffs) {
    ifstream fin(filePath.c_str(), ios::in);
    
    cameraMatix = cv::Mat(3, 3, CV_64FC1);
    fin >> cameraMatix.at<double>(0, 0);
    fin >> cameraMatix.at<double>(0, 1);
    fin >> cameraMatix.at<double>(0, 2);
    fin >> cameraMatix.at<double>(1, 0);
    fin >> cameraMatix.at<double>(1, 1);
    fin >> cameraMatix.at<double>(1, 2);
    fin >> cameraMatix.at<double>(2, 0);
    fin >> cameraMatix.at<double>(2, 1);
    fin >> cameraMatix.at<double>(2, 2);
    
    distCoeffs = cv::Mat(1, 4, CV_64FC1);
    fin >> distCoeffs.at<double>(0, 0);
    fin >> distCoeffs.at<double>(0, 1);
    fin >> distCoeffs.at<double>(0, 2);
    fin >> distCoeffs.at<double>(0, 3);
    
//     cout << "cameraMatix:\n" << cameraMatix << endl;
//     cout << "distCoeffs:\n" << distCoeffs << endl;
    
    fin.close();
}

int main(int argc, char * argv[]) {
  
  cout << "Hello world!" << endl;
//     cv::Mat showImg1, showImg2;
//     
//     cv::namedWindow("showImg1");
//     cv::namedWindow("showImg2");
//     
//     calibrator.toggleShowCommonRegion(false);
//     calibrator.toggleShowIterations(false);
//     for (uint32_t i = MIN_IDX; i < MAX_IDX; i++) {
//         boost::filesystem::path img1Path(BASE_PATH);
//         boost::filesystem::path img2Path(BASE_PATH);
//         
//         char imageName[1024];
//         sprintf(imageName, FILE_STRING1, i);
//         img1Path /= IMG1_PATH;
//         img1Path /= imageName;
//         sprintf(imageName, FILE_STRING2, i + 1);
// //         sprintf(imageName, FILE_STRING2, i);
//         img2Path /= IMG2_PATH;
//         img2Path /= imageName;
//         
//         cout << img1Path.string() << endl;
//         cout << img2Path.string() << endl;
//         
//         cv::Mat img1distorted = cv::imread(img1Path.string(), 0);
//         cv::Mat img2distorted = cv::imread(img2Path.string(), 0);
//         
//         // Images are dedistorted
//         boost::filesystem::path calibrationPath1(BASE_PATH);
//         boost::filesystem::path calibrationPath2(BASE_PATH);
//         
//         char calibrationName[1024];
//         
//         sprintf(calibrationName, CALIBRATION_STRING, 1);
//         calibrationPath1 /= calibrationName;
//         sprintf(calibrationName, CALIBRATION_STRING, 2);
//         calibrationPath2 /= calibrationName;
//         
//         cout << calibrationPath1 << endl;
//         cout << calibrationPath2 << endl;
//         
//         cv::Mat cameraMatrix1, distCoeffs1, cameraMatrix2, distCoeffs2;
//         getCalibrationMatrix(calibrationPath1, cameraMatrix1, distCoeffs1);
//         getCalibrationMatrix(calibrationPath2, cameraMatrix2, distCoeffs2);
// 
//         cv::imshow("showImg1", showImg1);
//         cv::imshow("showImg2", showImg2);
//         cv::moveWindow("showImg2", 700, 0);
        
//         cv::waitKey(0);
//    }
  
  return 0;
}


