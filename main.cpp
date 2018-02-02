// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2017 Intel Corporation. All Rights Reserved.
#include <iostream>
#include <signal.h>

#include <librealsense/rs.hpp>
#include "rs_sdk.h"
#include "version.h"
#include "pt_utils.hpp"
#include "pt_console_display.hpp"
#include "main.h"

using namespace std;

// Version number of the samples
extern constexpr auto rs_sample_version = concat("VERSION: ",RS_SAMPLE_VERSION_STR);

int main(int argc, char** argv)
{
    state_e state = STATE_INIT;

    pt_utils pt_utils;
    unique_ptr<console_display::pt_console_display> console_view = move(console_display::make_console_pt_display());

    rs::core::video_module_interface::actual_module_config actualModuleConfig;
    rs::person_tracking::person_tracking_video_module_interface* ptModule = nullptr;


    // Initializing Camera and Person Tracking modules
    if(pt_utils.init_camera(actualModuleConfig) != rs::core::status_no_error)
    {
        cerr << "Error: Device is null." << endl << "Please connect a RealSense device and restart the application" << endl;
        return -1;
    }
    pt_utils.init_person_tracking(&ptModule);

    //Enable Pointing Gesture
    ptModule->QueryConfiguration()->QueryGestures()->Enable();
    ptModule->QueryConfiguration()->QueryGestures()->EnableAllGestures();
    ptModule->QueryConfiguration()->QueryTracking()->Enable();
    ptModule->QueryConfiguration()->QuerySkeletonJoints()->Enable();


    // Configure enabled Pointing Gesture
    if(ptModule->set_module_config(actualModuleConfig) != rs::core::status_no_error)
    {
        cerr<<"Error : Failed to configure the enabled Pointing Gesture" << endl;
        return -1;
    }

    // Start the camera
    pt_utils.start_camera();

    cout << endl << "-------- Press Esc key to exit --------" << endl << endl;

    state = STATE_IDLE;
    Intel::RealSense::PersonTracking::PersonTrackingData *trackingData = ptModule->QueryOutput();
    Intel::RealSense::PersonTracking::PersonTrackingData::PersonJoints *personJoints = nullptr;

    // Start main loop
    while(!pt_utils.user_request_exit())
    {
        rs::core::correlated_sample_set sampleSet = {};

        // Get next frame
        if (pt_utils.GetNextFrame(sampleSet) != 0)
        {
            cerr << "Error: Invalid frame" << endl;
            continue;
        }

        // Process frame
        if (ptModule->process_sample_set(sampleSet) != rs::core::status_no_error)
        {
            cerr << "Error : Failed to process sample" << endl;
            continue;
        }


        // Display color image
        auto colorImage = sampleSet[rs::core::stream_type::color];
        console_view->render_color_frames(colorImage);

        // Release color and depth image
        sampleSet.images[static_cast<uint8_t>(rs::core::stream_type::color)]->release();
        sampleSet.images[static_cast<uint8_t>(rs::core::stream_type::depth)]->release();

        // Main program FSM implementation
        int numTracked;
        switch (state)
        {
            case STATE_IDLE:
                cout << "In idle state" << endl;
                numTracked = trackingData->QueryNumberOfPeople();

                if(numTracked == 1)
                {
                    // If we are tracking exactly one person, detect their gesture
                    console_view->set_tracking(ptModule);
                    state = STATE_READY;
                }

            break;

            case STATE_READY:
            // Track skeleton joints
            if(trackingData->QueryNumberOfPeople() == 0)
            {
                // If we no longer see a person, back to idle state
                state = STATE_IDLE;
            }
            else
            {
                // Start tracking the first person detected in the frame
                console_view->on_person_skeleton(ptModule);
                personJoints = console_view->on_person_skeleton(ptModule);
                detectGestures(personJoints);
            }

            break;

        case STATE_PLAYBACK:
            // @TODO Issue system call to playback video content over HDMI
            // system(...);

            // @TODO
            // If we are still detecting a person, listen for cancel gesture
            break;



        }

    }

    pt_utils.stop_camera();
    actualModuleConfig.projection->release();
    cout << "-------- Stopping --------" << endl;
    return 0;
}

gestures_e detectGestures(Intel::RealSense::PersonTracking::PersonTrackingData::PersonJoints *personJoints)
{


    int numDetectedJoints = personJoints->QueryNumJoints();
    std::vector<Intel::RealSense::PersonTracking::PersonTrackingData::PersonJoints::SkeletonPoint> skeletonPoints(personJoints->QueryNumJoints());

    personJoints->QueryJoints(skeletonPoints.data());

         jointCoords_t jointCoords;

    for(int i = 0; i < numDetectedJoints ; i++) {
                     // Populate joint coordinates values
                     jointCoords.Lhandx = skeletonPoints.at(0).image.x;
                     jointCoords.Lhandy = skeletonPoints.at(0).image.y;
                     jointCoords.Rhandx = skeletonPoints.at(1).image.x;
                     jointCoords.Rhandy = skeletonPoints.at(1).image.y;

                     jointCoords.Lshoulderx = skeletonPoints.at(4).image.x;
                     jointCoords.Lshouldery = skeletonPoints.at(4).image.y;
                     jointCoords.Rshoulderx = skeletonPoints.at(5).image.x;
                     jointCoords.Rshouldery = skeletonPoints.at(5).image.y;


                     //printJointCoords(jointCoords);

                 // check for each pose sequentially.
                 // some way to make this cleaner, perhaps a helper function with just jointCoords struct as only parameter?

                 //Power pose
                 if( ((jointCoords.Lshoulderx - jointCoords.Lhandx) <= 40) &&
                     ((jointCoords.Lshoulderx - jointCoords.Lhandx) > 0 ) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) <= 40) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) > 0) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) <= 0) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) > -40 ) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) <= 40) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) > 0)
                   )
                 {
                         cout << "Power Pose Detected!" << endl << endl;
                         //system("firefox goo.gl/xZPVb9");
                         return GESTURE_POWERPOSE;
                 }

                 if( ((jointCoords.Lshoulderx - jointCoords.Lhandx) <= 30) &&
                     ((jointCoords.Lshoulderx - jointCoords.Lhandx) >= 10 ) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) <= 120) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) >= 100) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) <= 0) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) >= -20 ) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) <= 120) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) >= 100)
                   )
                 {
                        cout << "Touch the Sky Pose Detected!" << endl << endl;
                        //system("firefox goo.gl/xipLSq");
                        return GESTURE_SKY;
                 }

                 //Usain Bolt Pose (to the left)
                 if( ((jointCoords.Lshoulderx - jointCoords.Lhandx) <= 20) &&
                     ((jointCoords.Lshoulderx - jointCoords.Lhandx) >= -20 ) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) <= -20) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) >= -60) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) <= -50) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) >= -100 ) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) <= 50) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) >= -10)
                   )
                 {
                        cout << "Usain Bolt Pose Detected!" << endl << endl;
                        //system("firefox goo.gl/xipLSq");
                        return GESTURE_USAIN;
                 }

                 if( ((jointCoords.Lshoulderx - jointCoords.Lhandx) <= 90) &&
                     ((jointCoords.Lshoulderx - jointCoords.Lhandx) >= 70 ) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) <= -20) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) >= -30) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) <= -90) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) >= -110 ) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) <= 0) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) >= -20)
                   )
                 {
                        cout << "T Pose Detected!" << endl << endl;
                        //system("firefox goo.gl/xipLSq");
                        return GESTURE_T;
                 }

                 if( ((jointCoords.Lshoulderx - jointCoords.Lhandx) <= 5) &&
                     ((jointCoords.Lshoulderx - jointCoords.Lhandx) >= -35 ) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) <= 25) &&
                     ((jointCoords.Lshouldery - jointCoords.Lhandy) >= 5) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) <= 5) &&
                     ((jointCoords.Rshoulderx - jointCoords.Rhandx) >= -15 ) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) <= 5) &&
                     ((jointCoords.Rshouldery - jointCoords.Rhandy) >= -15)
                   )
                 {
                        cout << "O Pose Detected!" << endl << endl;
                        //system("firefox goo.gl/xipLSq");
                        return GESTURE_0;
                 }

    }

    return GESTURE_UNDEFINED;
}

void printJointCoords(jointCoords_t jc)
{
    // 6: LH, 7: RH, 10: H, 19: S, 16: LS, 17: RS
    cout << "LH: " << jc.Lhandx << ", " << jc.Lhandy
         << "|RH: " << jc.Rhandx << ", " << jc.Rhandy
         << "|LS: " << jc.Lshoulderx << ", " << jc.Lshouldery
         << "|RS: " << jc.Rshoulderx << ", " << jc.Rshouldery
         //<< "|H: " << skeletonPoints.at(2).image.x << ", " << skeletonPoints.at(2).image.y
         //<< "|S: " << skeletonPoints.at(3).image.x << ", " << skeletonPoints.at(3).image.y
         << endl;
}
