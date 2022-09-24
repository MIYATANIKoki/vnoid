﻿#pragma once

#include "robot.h"
#include "fksolver.h"
#include "iksolver.h"

/*
 * Inverse kinematics demo 
 * 
 * 
 */

namespace cnoid{
namespace vnoid{

class MyRobot : public Robot{
public:
	Timer          timer;
    Param          param;
    Base           base;
    Centroid       centroid;
    vector<Hand>   hand;
    vector<Foot>   foot;
    vector<Joint>  joint;
    
    FkSolver       fk_solver;
    IkSolver       ik_solver;

    int  marker_index;
    int  num_markers;

public:
	virtual void  Init   (SimpleControllerIO* io);
	virtual void  Control();
	
	MyRobot();

};

}
}