// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2017 Intel Corporation. All Rights Reserved.

// C++ std libraries
#include <iostream>
#include <signal.h>
#include <thread>

// Boost Libraries
#include <boost/interprocess/ipc/message_queue.hpp>

// Realsense libraries
#include <librealsense/rs.hpp>
#include "rs_sdk.h"
#include "version.h"
#include "pt_utils.hpp"
#include "pt_console_display.hpp"


#include "main.h"



using namespace std;
using namespace boost::interprocess;

// Version number of the samples
extern constexpr auto rs_sample_version = concat("VERSION: ",RS_SAMPLE_VERSION_STR);

//Test
std::vector<jointCoords_t> arr(30);
std::vector<jointCoords_t>::iterator it;
bool detected = false;


int main(int argc, char** argv)
{
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
    Intel::RealSense::PersonTracking::PersonTrackingData *trackingData = ptModule->QueryOutput();
    Intel::RealSense::PersonTracking::PersonTrackingData::PersonJoints *personJoints = nullptr;

    // Init interprocess message queue
    message_queue::remove("etree_message_queue");
    message_queue mq
            (create_only,
             "etree_message_queue",
             1,
             sizeof(bool));
    unsigned int priority;
    message_queue::size_type recvd_size;

    state_e state = STATE_IDLE;
    int numTracked;
    gestures_e gestureDetected = GESTURE_UNDEFINED;
    bool playbackFinished = false;
    gesture_states_t gesture_states;
    resetGestureStates(gesture_states);

    // Start main loop
    while(!pt_utils.user_request_exit())
    {
        // Check for cancel request from system
        bool shouldQuit = false;
        mq.try_receive(&shouldQuit, sizeof(shouldQuit), recvd_size, priority);
        if(shouldQuit) break;

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
        switch (state)
        {
        case STATE_IDLE:
            numTracked = trackingData->QueryNumberOfPeople();
            //cout << "In idle state. " << numTracked << " people detected." << endl;

            if(numTracked == 1) // numTracked >= 1?
            {
                // If we are tracking exactly one person, detect their gesture
                cout << "found someone!" << endl;
                int personId = trackingData->QueryPersonData(
                            Intel::RealSense::PersonTracking::PersonTrackingData::ACCESS_ORDER_BY_INDEX, 0)->QueryTracking()->QueryId();
                cout << "ID before clearing database: " << personId << endl;

                auto config = ptModule->QueryConfiguration()->QueryRecognition();
                auto database = config->QueryDatabase();
                database->Clear();

                personId = trackingData->QueryPersonData(
                            Intel::RealSense::PersonTracking::PersonTrackingData::ACCESS_ORDER_BY_INDEX, 0)->QueryTracking()->QueryId();
                cout << "ID after clearing database: " << personId << endl;

                if(personId == 0)
                {
                    console_view->set_tracking(ptModule);
                    state = STATE_READY;
                    gesture_states.flying_gesture_state = gesture_states.FLYING_MAX_1;
                }
            }

            break;

        case STATE_READY:
            // Track skeleton joints
            if(trackingData->QueryNumberOfPeople() != 1)
            {
                // If we no longer see a person, back to idle state
                state = STATE_IDLE;
                //trackingData->StopTracking(0);

            }
            else
            {
                // Start tracking the first person detected in the frame
                personJoints = console_view->on_person_skeleton(ptModule);
                gestureDetected = detectGestures(personJoints, gesture_states);

                if(gestureDetected != GESTURE_UNDEFINED && gestureDetected != GESTURE_CANCEL)
                {
                    state = STATE_PLAYBACK_START;
                }
            }

            break;

        case STATE_PLAYBACK_START:
            // Issue system call to playback video content in a detached thread
            playbackFinished = false;
        {
            thread video(playContent, gestureDetected, ref(playbackFinished));
            video.detach();
        }

            state = STATE_PLAYBACK_UNDERWAY;
            break;

        case STATE_PLAYBACK_UNDERWAY:
            // If we are still detecting a person, listen for cancel gesture
            numTracked = trackingData->QueryNumberOfPeople();

            if(numTracked == 1)
            {
                personJoints = console_view->on_person_skeleton(ptModule);
                gestureDetected = detectGestures(personJoints, gesture_states);

                // @TODO Implement cancel gesture. Currently playback will cancel as soon as a person is tracked.
                if(gestureDetected == GESTURE_CANCEL)
                {
                    system("killall vlc");
                }
            }

            if(playbackFinished)
            {
                cout << "playback completed or killed!" << endl;
                state = STATE_READY;
            }
            break;
        }
    }

    pt_utils.stop_camera();
    actualModuleConfig.projection->release();
    cout << "-------- Stopping --------" << endl;
    return 0;
}

gestures_e detectGestures(Intel::RealSense::PersonTracking::PersonTrackingData::PersonJoints *personJoints, gesture_states_t &gesture_states)
{
    int numDetectedJoints = personJoints->QueryNumJoints();
    std::vector<Intel::RealSense::PersonTracking::PersonTrackingData::PersonJoints::SkeletonPoint> skeletonPoints(personJoints->QueryNumJoints());

    personJoints->QueryJoints(skeletonPoints.data());

    jointCoords_t jointCoords;

    //cout << "num detected joints" << numDetectedJoints << endl;

    //cout << skeletonPoints.at(0).image.x << endl;


    // Populate joint coordinates values
    jointCoords.Lhandx = skeletonPoints.at(0).image.x;
    jointCoords.Lhandy = skeletonPoints.at(0).image.y;
    jointCoords.Rhandx = skeletonPoints.at(1).image.x;
    jointCoords.Rhandy = skeletonPoints.at(1).image.y;

    jointCoords.Lshoulderx = skeletonPoints.at(4).image.x;
    jointCoords.Lshouldery = skeletonPoints.at(4).image.y;
    jointCoords.Rshoulderx = skeletonPoints.at(5).image.x;
    jointCoords.Rshouldery = skeletonPoints.at(5).image.y;

    jointCoords.headx = skeletonPoints.at(2).image.x;
    jointCoords.heady = skeletonPoints.at(2).image.y;
    jointCoords.Spinex = skeletonPoints.at(3).image.x;
    jointCoords.Spiney = skeletonPoints.at(3).image.y;


    // check for each pose sequentially.
    // some way to make this cleaner, perhaps a helper function with just jointCoords struct as only parameter?

    int LeftX = jointCoords.Lshoulderx - jointCoords.Lhandx;
    int LeftY = jointCoords.Lshouldery - jointCoords.Lhandy;
    int RightX = jointCoords.Rshoulderx - jointCoords.Rhandx;
    int RightY = jointCoords.Rshouldery - jointCoords.Rhandy;


    // usain Pose
    switch(gesture_states.usain_gesture_state)
    {
    case gesture_states.USAIN_INIT:
        if( (LeftX <= 45) &&
                (LeftX >= 20 ) &&
                (LeftY <= -10) &&
                (LeftY >= -45) &&
                (RightX <= -30) &&
                (RightX >= -90 ) &&
                (RightY <= 50) &&
                (RightY >= 15)
                )
        {
            gesture_states.usain_gesture_state = gesture_states.USAIN_DETECTING;
            cout << "Detecting Usain-pose" << endl;

            gesture_states.cyclesInState_usain_detecting = 0;
        }
        break;
    case gesture_states.USAIN_DETECTING:
        if( (LeftX <= 45) &&
                (LeftX >= 20 ) &&
                (LeftY <= -10) &&
                (LeftY >= -45) &&
                (RightX <= -30) &&
                (RightX >= -90 ) &&
                (RightY <= 50) &&
                (RightY >= 15)
                )
        {
            gesture_states.cyclesInState_usain_detecting++;
            cout << "Detecting Usain-pose, " << gesture_states.cyclesInState_usain_detecting << "cycles" << endl;

            if(gesture_states.cyclesInState_usain_detecting >= STATIC_POSE_DETECTING_TIMEOUT)
            {
                cout << "Usain Pose Detected!" << endl << endl;
                resetGestureStates(gesture_states);
                return GESTURE_USAIN;
            }
        }
        else
        {
            gesture_states.usain_gesture_state = gesture_states.USAIN_LOST;
            cout << "Detecting Usain-post lost!!!!!!!!!" << endl;

            gesture_states.cyclesInState_usain_lost = 0;
        }
        break;

    case gesture_states.USAIN_LOST:
        //cout << "Lost Usain-pose! " << gesture_states.cyclesInState_usain_lost << "cycles" << endl;

        if( (LeftX <= 45) &&
                (LeftX >= 20 ) &&
                (LeftY <= -10) &&
                (LeftY >= -45) &&
                (RightX <= -30) &&
                (RightX >= -90 ) &&
                (RightY <= 50) &&
                (RightY >= 15)
                )
        {
            gesture_states.usain_gesture_state = gesture_states.USAIN_DETECTING;
            gesture_states.cyclesInState_usain_lost = 0;
        }
        else if(gesture_states.cyclesInState_usain_lost >= STATIC_POSE_LOST_TIMEOUT)
        {
            gesture_states.usain_gesture_state = gesture_states.USAIN_INIT;
            gesture_states.cyclesInState_usain_detecting = 0;
            gesture_states.cyclesInState_usain_lost = 0;
        }
        else
        {
            gesture_states.cyclesInState_usain_lost++;
        }
        break;
    }


    // Victory Pose
    switch(gesture_states.victory_gesture_state)
    {
    case gesture_states.VICTORY_INIT:
        if( (LeftX <= 70) &&
                (LeftX >= 35 ) &&
                (LeftY <= 90) &&
                (LeftY >= 50) &&
                (RightX <= -20) &&
                (RightX >= -60 ) &&
                (RightY <= 80) &&
                (RightY >= 50)
                )
        {
            gesture_states.victory_gesture_state = gesture_states.VICTORY_DETECTING;
            cout << "Detecting V-pose" << endl;

            gesture_states.cyclesInState_victory_detecting = 0;
        }
        break;
    case gesture_states.VICTORY_DETECTING:
        if( (LeftX <= 70) &&
                (LeftX >= 35 ) &&
                (LeftY <= 90) &&
                (LeftY >= 50) &&
                (RightX <= -20) &&
                (RightX >= -60 ) &&
                (RightY <= 80) &&
                (RightY >= 50)
                )
        {
            gesture_states.cyclesInState_victory_detecting++;
            cout << "Detecting V-pose, " << gesture_states.cyclesInState_victory_detecting << "cycles" << endl;

            if(gesture_states.cyclesInState_victory_detecting >= STATIC_POSE_DETECTING_TIMEOUT)
            {
                cout << "V Pose Detected!" << endl << endl;
                resetGestureStates(gesture_states);
                return GESTURE_VICTORY;
            }
        }
        else
        {
            gesture_states.victory_gesture_state = gesture_states.VICTORY_LOST;
            cout << "Detecting V-post lost!!!!!!!!!" << endl;

            gesture_states.cyclesInState_victory_lost = 0;
        }
        break;

    case gesture_states.VICTORY_LOST:
        //cout << "Lost V-pose! " << gesture_states.cyclesInState_vpose_lost << "cycles" << endl;

        if( (LeftX <= 70) &&
                (LeftX >= 35 ) &&
                (LeftY <= 90) &&
                (LeftY >= 50) &&
                (RightX <= -20) &&
                (RightX >= -60 ) &&
                (RightY <= 80) &&
                (RightY >= 50)
                )
        {
            gesture_states.victory_gesture_state = gesture_states.VICTORY_DETECTING;
            gesture_states.cyclesInState_victory_lost = 0;
        }
        else if(gesture_states.cyclesInState_victory_lost >= STATIC_POSE_LOST_TIMEOUT)
        {
            gesture_states.victory_gesture_state = gesture_states.VICTORY_INIT;
            gesture_states.cyclesInState_victory_detecting = 0;
            gesture_states.cyclesInState_victory_lost = 0;
        }
        else
        {
            gesture_states.cyclesInState_victory_lost++;
        }
        break;
    }



    // Power Pose
    switch(gesture_states.powerpose_gesture_state)
    {
    case gesture_states.POWERPOSE_INIT:
        if( (LeftX <= 40) &&
                (LeftX > 0 ) &&
                (LeftY <= 40) &&
                (LeftY > 0) &&
                (RightX <= 0) &&
                (RightX > -40 ) &&
                (RightY <= 40) &&
                (RightY > 0)
                )
        {
            gesture_states.powerpose_gesture_state = gesture_states.POWERPOSE_DETECTING;
            cout << "Detecting Power-pose" << endl;

            gesture_states.cyclesInState_powerpose_detecting = 0;
        }
        break;
    case gesture_states.POWERPOSE_DETECTING:
        if( (LeftX <= 40) &&
                (LeftX > 0 ) &&
                (LeftY <= 40) &&
                (LeftY > 0) &&
                (RightX <= 0) &&
                (RightX > -40 ) &&
                (RightY <= 40) &&
                (RightY > 0)
                )
        {
            gesture_states.cyclesInState_powerpose_detecting++;
            cout << "Detecting Power-pose, " << gesture_states.cyclesInState_powerpose_detecting << "cycles" << endl;

            if(gesture_states.cyclesInState_powerpose_detecting >= STATIC_POSE_DETECTING_TIMEOUT)
            {
                cout << "Power Pose Detected!" << endl << endl;
                resetGestureStates(gesture_states);
                return GESTURE_POWERPOSE;
            }
        }
        else
        {
            gesture_states.powerpose_gesture_state = gesture_states.POWERPOSE_LOST;
            cout << "Detecting P-post lost!!!!!!!!!" << endl;

            gesture_states.cyclesInState_powerpose_lost = 0;
        }
        break;

    case gesture_states.POWERPOSE_LOST:
        //cout << "Lost P-pose! " << gesture_states.cyclesInState_powerpose_lost << "cycles" << endl;

        if( (LeftX <= 40) &&
                (LeftX > 0 ) &&
                (LeftY <= 40) &&
                (LeftY > 0) &&
                (RightX <= 0) &&
                (RightX > -40 ) &&
                (RightY <= 40) &&
                (RightY > 0)
                )
        {
            gesture_states.powerpose_gesture_state = gesture_states.POWERPOSE_DETECTING;
            gesture_states.cyclesInState_powerpose_lost = 0;
        }
        else if(gesture_states.cyclesInState_powerpose_lost >= STATIC_POSE_LOST_TIMEOUT)
        {
            gesture_states.powerpose_gesture_state = gesture_states.POWERPOSE_INIT;
            gesture_states.cyclesInState_powerpose_detecting = 0;
            gesture_states.cyclesInState_powerpose_lost = 0;
        }
        else
        {
            gesture_states.cyclesInState_powerpose_lost++;
        }
        break;
    }



    // T Pose Gesture
    switch(gesture_states.tpose_gesture_state)
    {
    case gesture_states.TPOSE_INIT:
        if( //(LeftX <= 100) &&
                (LeftX >= 75 ) &&
                (LeftY <= 10) &&
                (LeftY >= -10) &&
                //(RightX <= -80) &&
                (RightX >= -110 ) &&
                (RightY <= 5) &&
                (RightY >= -15)
                )
        {
            gesture_states.tpose_gesture_state = gesture_states.TPOSE_DETECTING;
            cout << "Detecting T-pose11111111111111111111111111" << endl;

            gesture_states.cyclesInState_tpose_detecting = 0;
        }
        break;
    case gesture_states.TPOSE_DETECTING:
        if( //(LeftX <= 100) &&
                (LeftX >= 75 ) &&
                (LeftY <= 10) &&
                (LeftY >= -10) &&
                //(RightX <= -80) &&
                (RightX >= -110 ) &&
                (RightY <= 5) &&
                (RightY >= -15)
                )
        {
            gesture_states.cyclesInState_tpose_detecting++;
            cout << "Detecting T-pose, " << gesture_states.cyclesInState_tpose_detecting << "cycles" << endl;

            if(gesture_states.cyclesInState_tpose_detecting >= STATIC_POSE_DETECTING_TIMEOUT)
            {
                cout << "T Pose Detected!" << endl << endl;
                resetGestureStates(gesture_states);
                return GESTURE_T;
            }
        }
        else
        {
            gesture_states.tpose_gesture_state = gesture_states.TPOSE_LOST;
            cout << "Detecting T- lost!!!!!!!!!" << endl;

            gesture_states.cyclesInState_tpose_lost = 0;
        }
        break;

    case gesture_states.TPOSE_LOST:
        //cout << "Lost T-pose! " << gesture_states.cyclesInState_tpose_lost << "cycles" << endl;

        if( //(LeftX <= 100) &&
                (LeftX >= 75 ) &&
                (LeftY <= 10) &&
                (LeftY >= -10) &&
                //(RightX <= -80) &&
                (RightX >= -110 ) &&
                (RightY <= 5) &&
                (RightY >= -15)
                )
        {
            gesture_states.tpose_gesture_state = gesture_states.TPOSE_DETECTING;
            gesture_states.cyclesInState_tpose_lost = 0;
        }
        else if(gesture_states.cyclesInState_tpose_lost >= STATIC_POSE_LOST_TIMEOUT)
        {
            gesture_states.tpose_gesture_state = gesture_states.TPOSE_INIT;
            gesture_states.cyclesInState_tpose_detecting = 0;
            gesture_states.cyclesInState_tpose_lost = 0;
        }
        else
        {
            gesture_states.cyclesInState_tpose_lost++;
        }
        break;
    }

    /*
                 // O Pose Gesture
                 if( (LeftX <= 15) &&
                     (LeftX >= -10 ) &&
                     (LeftY <= 65) &&
                     (LeftY >= 55) &&
                     (RightX <= 15) &&
                     (RightX >= -5 ) &&
                     (RightY <= 70) &&
                     (RightY >= 55)
                   )
                 {
                        cout << "O Pose Detected!" << endl << endl;
                        return GESTURE_0;
                 }
*/

    //max
    switch(gesture_states.flying_gesture_state)
    {
    case gesture_states.FLYING_INIT:
        if((LeftY >= -20) &&
                (LeftY <= 20) && //15
                (RightY >= -10) &&
                (RightY <= 30))
        {
            gesture_states.cyclesInState_flying = 0;
            gesture_states.flying_gesture_state = gesture_states.FLYING_MAX_1;
        }
        //cout << "FLYING_INIT" << endl;
        break;

    case gesture_states.FLYING_MAX_1:
        if((LeftY >= -100) &&
                (LeftY <= -60) &&
                (RightY >= -90) &&
                (RightY <= -50))
        {
            gesture_states.cyclesInState_flying = 0;
            gesture_states.flying_gesture_state = gesture_states.FLYING_MIN_1;
        }
        else if(gesture_states.cyclesInState_flying >= FLYING_TIMEOUT){
            gesture_states.cyclesInState_flying = 0;
            gesture_states.flying_gesture_state = gesture_states.FLYING_INIT;
        }
        else
        {
            gesture_states.cyclesInState_flying++;
        }
        //cout << "FLYING_MAX_1" << endl;
        break;

    case gesture_states.FLYING_MIN_1:
        if((LeftY >= -20) &&
                (LeftY <= 20) && //15
                (RightY >= -10) &&
                (RightY <= 30))
        {
            gesture_states.cyclesInState_flying = 0;
            gesture_states.flying_gesture_state = gesture_states.FLYING_MAX_2;
        }
        else if(gesture_states.cyclesInState_flying >= FLYING_TIMEOUT){
            gesture_states.cyclesInState_flying = 0;
            gesture_states.flying_gesture_state = gesture_states.FLYING_INIT;
        }
        else
        {
            gesture_states.cyclesInState_flying++;
        }
        //cout << "FLYING_MIN_1" << endl;
        break;

    case gesture_states.FLYING_MAX_2:
        if((LeftY >= -100) &&
                (LeftY <= 0) &&
                (RightY >= -90) &&
                (RightY <= -50))
        {
            cout << "Flying gesture detected!" << endl;
            gesture_states.flying_gesture_state = gesture_states.FLYING_INIT;
            return GESTURE_FLYING;
        }
        else if(gesture_states.cyclesInState_flying >= FLYING_TIMEOUT){
            gesture_states.cyclesInState_flying = 0;
            gesture_states.flying_gesture_state = gesture_states.FLYING_INIT;
        }
        else
        {
            gesture_states.cyclesInState_flying++;
        }
        //cout << "FLYING_MAX_2" << endl;
        break;

    default:
        break;
    }


    // Don't delete. Will clean up code
    /*
    it = arr.begin();
    arr.insert(it, jointCoords);
    bool boolArr[10] = {false};
//    for(int i = 0; i < 5; i++) {
//        printJointCoords(arr.at(i));
//    }

    for(int i = 10; i < 20; i++) {
        if(arr.at(0).heady - arr.at(i).heady >= 30) {
            boolArr[i-10] = true;
        } else {
            boolArr[i-10] = false;
        }
    }

    for(int i = 0; i < 10; i++) {
        cout <<  boolArr[i] << ", ";
        if( boolArr[i] == 0) {
            detected = false;
            break;
        } else {
            detected = true;
        }
    }
cout << endl;
    if(detected == true) {
        cout << "SUCCESS!" << endl << endl;
        detected = false;
    }
*/

    //    printJointCoords(jointCoords);

    return GESTURE_UNDEFINED;
}

void printJointCoords(jointCoords_t& jc)
{
    // 6: LH, 7: RH, 10: H, 19: S, 16: LS, 17: RS
    cout << "LH: " << jc.Lhandx << ", " << jc.Lhandy
         << "|RH: " << jc.Rhandx << ", " << jc.Rhandy
         << "|LS: " << jc.Lshoulderx << ", " << jc.Lshouldery
         << "|RS: " << jc.Rshoulderx << ", " << jc.Rshouldery
            //<< "|H: " << jc.headx << ", " << jc.heady
            //<< "|S: " << jc.Spinex << ", " << jc.Spiney
         << endl;
}

void playContent(gestures_e gesture, bool &finished)
{
    // Play the specified video in fullscreen mode and close vlc when finished
    // (We should use this in our production code)
    //    system("cvlc -f --play-and-exit file:///home/zac/electricTree/videos/test.mov");

    // Play the specified video on a loop (useful for testing cancel gesture)
    system("cvlc -R file:///home/zac/electricTree/videos/test.mov");

    // Set finished to true. This is a reference so its value will be seen in the main loop.
    finished = true;
    cout << "playback completed! " << endl;
}

void resetGestureStates(gesture_states_t &gesture_states)
{
    gesture_states.usain_gesture_state = gesture_states.USAIN_INIT;
    gesture_states.tpose_gesture_state = gesture_states.TPOSE_INIT;
    gesture_states.o_gesture_state = gesture_states.O_INIT;
    gesture_states.victory_gesture_state = gesture_states.VICTORY_INIT;
    gesture_states.powerpose_gesture_state = gesture_states.POWERPOSE_INIT;
    gesture_states.flying_gesture_state = gesture_states.FLYING_INIT;
}
