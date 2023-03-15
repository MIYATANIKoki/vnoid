﻿#include "stabilizer.h"

#include "robot.h"
#include "rollpitchyaw.h"
#include "footstep.h"

namespace cnoid{
namespace vnoid{

Stabilizer::Stabilizer(){
    min_contact_force        = 1.0;
	force_ctrl_damping       = 0.0;
	force_ctrl_gain          = 0.0;
	force_ctrl_limit         = 0.0;
	moment_ctrl_damping      = 0.0;
	moment_ctrl_gain         = 0.0;
	moment_ctrl_limit        = 0.0;
	
	// default gain setting
	orientation_ctrl_gain_p = 10.0;
	orientation_ctrl_gain_d = 10.0;
	dcm_ctrl_gain_p           = 10.0;
	dcm_ctrl_gain_i           = 10.0;
	zmp_ctrl_gain             = 10.0;

	//
	recovery_moment_limit = 100.0;
	dcm_deviation_limit   = 0.3;
	
    for(int i = 0; i < 2; i++){
        dpos[i] = Vector3(0.0, 0.0, 0.0);
	    drot[i] = Vector3(0.0, 0.0, 0.0);
    }

}

void Stabilizer::CalcZmp(const Param& param, Centroid& centroid, vector<Foot>& foot){
    // get actual force from the sensor
	for(int i = 0; i < 2; i++){
		// set contact state
		foot[i].contact = (foot[i].force.z() >= min_contact_force);

		// measure continuous contact duration
		if(foot[i].contact){
			foot[i].zmp = Vector3(-foot[i].moment.y()/foot[i].force.z(), foot[i].moment.x()/foot[i].force.z(), 0.0);
		}
		else{
			foot[i].zmp = Vector3(0.0, 0.0, 0.0);
		}
	}

	// both feet not in contact
	if(!foot[0].contact && !foot[1].contact){
		foot[0].balance = 0.5;
		foot[1].balance = 0.5;
		centroid.zmp = Vector3(0.0, 0.0, 0.0);
	}
	else{
		double f0 = std::max(0.0, foot[0].force.z());
		double f1 = std::max(0.0, foot[1].force.z());
		foot[0].balance = f0/(f0 + f1);
		foot[1].balance = f1/(f0 + f1);
		centroid.zmp =
			     (foot[0].balance) * (foot[0].pos_ref + foot[0].ori_ref * foot[0].zmp)
	           + (foot[1].balance) * (foot[1].pos_ref + foot[1].ori_ref * foot[1].zmp);
	}
}

void Stabilizer::CalcForceDistribution(const Param& param, Centroid& centroid, vector<Foot>& foot){
	// switch based on contact state
	if(!foot[0].contact_ref && !foot[1].contact_ref){
		foot[0].balance_ref = 0.5;
		foot[1].balance_ref = 0.5;
		foot[0].zmp_ref = Vector3(0.0, 0.0, 0.0);
		foot[1].zmp_ref = Vector3(0.0, 0.0, 0.0);
	}
	if( foot[0].contact_ref && !foot[1].contact_ref){
		foot[0].balance_ref = 1.0;
		foot[1].balance_ref = 0.0;
		foot[0].zmp_ref = foot[0].ori_ref.conjugate() * (centroid.zmp_ref - foot[0].pos_ref);
		foot[1].zmp_ref = Vector3(0.0, 0.0, 0.0);
	}
	if(!foot[0].contact_ref &&  foot[1].contact_ref){
		foot[0].balance_ref = 0.0;
		foot[1].balance_ref = 1.0;
		foot[0].zmp_ref = Vector3(0.0, 0.0, 0.0);
		foot[1].zmp_ref = foot[1].ori_ref.conjugate() * (centroid.zmp_ref - foot[1].pos_ref);
	}
	if( foot[0].contact_ref &&  foot[1].contact_ref){
		//
		Vector2 b;
		Vector3 pdiff  = foot[1].pos_ref - foot[0].pos_ref;
		double  pdiff2 = pdiff.squaredNorm();
		const double eps = 1.0e-10;
		if(pdiff2 < eps){
			b[0] = b[1] = 0.5;
		}
		else{
			b[0] = (pdiff.dot(foot[1].pos_ref - centroid.zmp_ref))/pdiff2;
			b[0] = std::min(std::max(0.0, b[0]), 1.0);
			b[1] = 1.0 - b[0];
		}

		foot[0].balance_ref = b[0];
		foot[1].balance_ref = b[1];

		Vector3 zmp_proj = b[0]*foot[0].pos_ref + b[1]*foot[1].pos_ref;

		double b2 = b.squaredNorm();
		foot[0].zmp_ref = (b[0]/b2) * (foot[0].ori_ref.conjugate() * (centroid.zmp_ref - zmp_proj));
		foot[1].zmp_ref = (b[1]/b2) * (foot[1].ori_ref.conjugate() * (centroid.zmp_ref - zmp_proj));
	}

	// limit zmp
	for(int i = 0; i < 2; i++){
		for(int j = 0; j < 3; j++){
			foot[i].zmp_ref[j] = std::min(std::max(param.zmp_min[j], foot[i].zmp_ref[j]), param.zmp_max[j]);
		}
	}

	for(int i = 0; i < 2; i++){
		// force and moment to realize desired Zmp
		foot[i].force_ref     =  foot[i].ori_ref.conjugate() * (foot[i].balance_ref * centroid.force_ref);
		foot[i].moment_ref[0] =  foot[i].force_ref.z() * foot[i].zmp_ref.y();
		foot[i].moment_ref[1] = -foot[i].force_ref.z() * foot[i].zmp_ref.x();
		foot[i].moment_ref[2] =  foot[i].balance_ref * centroid.moment_ref.z();

		foot[i].moment_ref += foot[i].balance_ref * (foot[i].ori_ref.conjugate() * centroid.moment_ref);
	}
}

void Stabilizer::Predict(const Timer& timer, const Param& param, const Footstep& footstep_buffer, const Base& base, Centroid& centroid){
	const Step& stb0 = footstep_buffer.steps[0];
    
	double ttl = (stb0.tbegin + stb0.duration - timer.time);
	int N = (int)(ttl/timer.dt);

	Vector3 theta = base.angle  - base.angle_ref;
	Vector3 omega = base.angvel - base.angvel_ref;

	for(int k = 0; k < N-1; k++){
		// predicted update of base link orientation
		theta += omega*timer.dt;
		omega += -(orientation_ctrl_gain_p*theta + orientation_ctrl_gain_d*omega)*timer.dt;
		
		CalcDcmDynamics(timer, param, footstep_buffer, base, theta, omega, centroid);
	}
}



void Stabilizer::CalcDcmDynamics(const Timer& timer, const Param& param, const Footstep& footstep_buffer, const Base& base, Vector3 theta, Vector3 omega, Centroid& centroid){
	const Step& stb0 = footstep_buffer.steps[0];
    const Step& stb1 = footstep_buffer.steps[1];
    int sup =  stb0.side;
    int swg = !stb0.side;
	
	double T = param.T;
	double m = param.total_mass;
	double h = param.com_height;
	double T_mh  = T/(m*h);
	double T2_mh = T*T_mh;

	Vector3 offset(0.0, 0.0, param.com_height);
	
	// calc moment for regulating orientation
	Vector3 Ld = base.ori_ref * Vector3(
		-param.nominal_inertia.x()*(orientation_ctrl_gain_p*theta.x() + orientation_ctrl_gain_d*omega.x()),
		-param.nominal_inertia.y()*(orientation_ctrl_gain_p*theta.y() + orientation_ctrl_gain_d*omega.y()),
		//-param.nominal_inertia.z()*(/*orientation_ctrl_gain_p*theta.z() + */1.0*orientation_ctrl_gain_d*omega.z())
		0.0
	);

	// safe to limit recovery moment
	for(int i = 0; i < 3; i++){
		Ld[i] = std::min(std::max(-recovery_moment_limit, Ld[i]), recovery_moment_limit);
	}

	/*
	// test code. never mind!
	Vector3 r = centroid.com_pos_ref - stb0.zmp;
	Vector3 tz = r/r.norm();
	Vector3 tx;
	Vector3 ty(0.0, 1.0, 0.0);
	tx = ty.cross(tz);
	ty = tz.cross(tx);

	Eigen::Matrix<double,3,2> S;
	S.col(0) = tx;
	S.col(1) = ty;
	Matrix3 rc;
	rc(0,0) =  0.0;   rc(0,1) = -r.z(); rc(0,2) =  r.y();
	rc(1,0) =  r.z(); rc(1,1) =  0.0;   rc(1,2) = -r.x();
	rc(2,0) = -r.y(); rc(2,1) =  r.x(); rc(2,2) =  0.0;
	Matrix2 tmp = S.transpose()*rc*S;
	Vector3 delta = -(S*tmp.inverse()*S.transpose())*((T/m)*Ld);
	*/

	// virtual disturbance applied to DCM dynamics to generate desired recovery moment
	Vector3 delta = Vector3(-T_mh*Ld.y(), T_mh*Ld.x(), 0.0);

	// calc zmp for regulating dcm
	Vector3 dcm_tmp = centroid.com_pos + T * centroid.com_vel;
	double Ti = 20.0; // leaky integrator time constant
	centroid.dcm_error_int *= exp(-timer.dt / Ti);
	centroid.dcm_error_int += (stb0.dcm - dcm_tmp)*timer.dt;
	centroid.zmp_ref = stb0.zmp - dcm_ctrl_gain_p*(stb0.dcm - dcm_tmp) - dcm_ctrl_gain_i/Ti * centroid.dcm_error_int + zmp_ctrl_gain * (stb0.zmp - centroid.zmp);

	// project zmp inside support region
	if(stb0.stepping){
		Vector3 zmp_local = stb0.foot_ori[sup].conjugate()*(centroid.zmp_ref - stb0.foot_pos[sup]);
		for(int j = 0; j < 3; j++){
			zmp_local[j] = std::min(std::max(param.zmp_min[j], zmp_local[j]), param.zmp_max[j]);
		}
		centroid.zmp_ref = stb0.foot_pos[sup] + stb0.foot_ori[sup]*zmp_local;
	}

	// calc DCM derivative
	Vector3 dcm_d = (1 / T) * (dcm_tmp - (centroid.zmp_ref + Vector3(0.0, 0.0, h)));

	// calc CoM acceleration
	centroid.com_acc_ref = (1 / T) * (dcm_d - centroid.com_vel);// -Vector3(20.0, 10.0, 0.0).cwiseProduct(centroid.zmp_ref - centroid.zmp);

	// update DCM
	centroid.dcm_ref += dcm_d*timer.dt;
	// limit deviation from reference dcm
	for(int j = 0; j < 3; j++){
		centroid.dcm_ref[j] = std::min(std::max(stb0.dcm[j] - dcm_deviation_limit, centroid.dcm_ref[j]), stb0.dcm[j] + dcm_deviation_limit);
	}

	// calc CoM velocity from dcm
	centroid.com_vel_ref = (1/T)*(centroid.dcm_ref - centroid.com_pos);

	// update CoM position
	centroid.com_pos_ref += centroid.com_vel_ref*timer.dt;
}

void Stabilizer::Update(const Timer& timer, const Param& param, const Footstep& footstep_buffer, Centroid& centroid, Base& base, vector<Foot>& foot){
	const Step& stb0 = footstep_buffer.steps[0];
    const Step& stb1 = footstep_buffer.steps[1];
    int sup =  stb0.side;
    int swg = !stb0.side;

	// calc zmp from forces
    CalcZmp(param, centroid, foot);

	Vector3 theta = base.angle  - base.angle_ref;
	Vector3 omega = base.angvel - base.angvel_ref;
	
	CalcDcmDynamics(timer, param, footstep_buffer, base, theta, omega, centroid);

	// calc desired force applied to CoM
	/*Vector3 pcom = body.calcCenterOfMass();
	Vector3 vcom = calcComVel(body);*/
	//centroid.zmp_ref += 0.34*(centroid.com_vel_ref - vcom) + 2.67 * (centroid.com_pos_ref - pcom);
	centroid.force_ref  = param.total_mass*(centroid.com_acc_ref + Vector3(0.0, 0.0, param.gravity));
	centroid.moment_ref = Vector3(0.0, 0.0, 0.0);
	

	{
		Vector3 m(0.0, 0.0, 0.0);
		for (int j = 0; j < 2; j++) {
			m[j] = -param.nominal_inertia[j]* orientation_ctrl_gain_p*(theta[j]) + orientation_ctrl_gain_d * (omega[j]);
		}

		centroid.moment_ref = base.ori_ref * m;
	}
	for (int i = 0; i < 3; i++) {
		centroid.moment_ref[i] = std::min(std::max(-recovery_moment_limit, centroid.moment_ref[i]), recovery_moment_limit);
	}

	// calculate desired forces from desired zmp
	CalcForceDistribution(param, centroid, foot);

	for(int i = 0; i < 2; i++){
		// ground reaction force control
		if( foot[i].contact ){
			for(int j = 0; j < 3; j++){
				dpos[i][j] += (-force_ctrl_damping*dpos[i][j] + force_ctrl_gain*(foot[i].force_ref[j] - foot[i].force[j]))*timer.dt;
				dpos[i][j] = std::min(std::max(-force_ctrl_limit, dpos[i][j]), force_ctrl_limit);

				drot[i][j] += (-moment_ctrl_damping*drot[i][j] + moment_ctrl_gain*(foot[i].moment_ref[j] - foot[i].moment[j]))*timer.dt;
				drot[i][j] = std::min(std::max(-moment_ctrl_limit, drot[i][j]), moment_ctrl_limit);
			}

			// feedback to desired foot pose
			foot[i].pos_ref   += -dpos[i];
			foot[i].angle_ref += -drot[i];
            foot[i].ori_ref = FromRollPitchYaw(foot[i].angle_ref);
		}
	}

}
}
}
