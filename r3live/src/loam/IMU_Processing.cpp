#include "IMU_Processing.hpp"
#define COV_OMEGA_NOISE_DIAG 1e-1
#define COV_ACC_NOISE_DIAG 0.4
#define COV_GYRO_NOISE_DIAG 0.2

#define COV_BIAS_ACC_NOISE_DIAG 0.05
#define COV_BIAS_GYRO_NOISE_DIAG 0.1

#define COV_START_ACC_DIAG 1e-1
#define COV_START_GYRO_DIAG 1e-1
// #define COV_NOISE_EXT_I2C_R (0.0 * 1e-3)
// #define COV_NOISE_EXT_I2C_T (0.0 * 1e-3)
// #define COV_NOISE_EXT_I2C_Td (0.0 * 1e-3)



double g_lidar_star_tim = 0;
ImuProcess::ImuProcess() : b_first_frame_( true ), imu_need_init_( true ), last_imu_( nullptr ), start_timestamp_( -1 )
{
    Eigen::Quaterniond q( 0, 1, 0, 0 );
    Eigen::Vector3d    t( 0, 0, 0 );
    init_iter_num = 1;
    cov_acc = Eigen::Vector3d( COV_START_ACC_DIAG, COV_START_ACC_DIAG, COV_START_ACC_DIAG );
    cov_gyr = Eigen::Vector3d( COV_START_GYRO_DIAG, COV_START_GYRO_DIAG, COV_START_GYRO_DIAG );
    mean_acc = Eigen::Vector3d( 0, 0, -9.805 );
    mean_gyr = Eigen::Vector3d( 0, 0, 0 );
    angvel_last = Zero3d;
    cov_proc_noise = Eigen::Matrix< double, DIM_OF_PROC_N, 1 >::Zero();
    // Lidar_offset_to_IMU = Eigen::Vector3d(0.0, 0.0, -0.0);
    // fout.open(DEBUG_FILE_DIR("imu.txt"),std::ios::out);
}

ImuProcess::~ImuProcess()
{ /**fout.close();*/
}

void ImuProcess::Reset()
{
    ROS_WARN( "Reset ImuProcess" );
    angvel_last = Zero3d;
    cov_proc_noise = Eigen::Matrix< double, DIM_OF_PROC_N, 1 >::Zero();

    cov_acc = Eigen::Vector3d( COV_START_ACC_DIAG, COV_START_ACC_DIAG, COV_START_ACC_DIAG );
    cov_gyr = Eigen::Vector3d( COV_START_GYRO_DIAG, COV_START_GYRO_DIAG, COV_START_GYRO_DIAG );
    mean_acc = Eigen::Vector3d( 0, 0, -9.805 );
    mean_gyr = Eigen::Vector3d( 0, 0, 0 );

    imu_need_init_ = true;
    b_first_frame_ = true;
    init_iter_num = 1;

    last_imu_ = nullptr;

    // gyr_int_.Reset(-1, nullptr);
    start_timestamp_ = -1;
    v_imu_.clear();
    IMU_pose.clear();

    cur_pcl_un_.reset( new PointCloudXYZINormal() );
}

void ImuProcess::IMU_Initial( const MeasureGroup &meas, StatesGroup &state_inout, int &N )
{
    /** 1. initializing the gravity, gyro bias, acc and gyro covariance
     ** 2. normalize the acceleration measurenments to unit gravity **/
    ROS_INFO( "IMU Initializing: %.1f %%", double( N ) / MAX_INI_COUNT * 100 );
    Eigen::Vector3d cur_acc, cur_gyr;

    if ( b_first_frame_ )
    {
        Reset();
        N = 1;
        b_first_frame_ = false;
    }

    for ( const auto &imu : meas.imu )
    {
        const auto &imu_acc = imu->linear_acceleration;
        const auto &gyr_acc = imu->angular_velocity;
        cur_acc << imu_acc.x, imu_acc.y, imu_acc.z;
        cur_gyr << gyr_acc.x, gyr_acc.y, gyr_acc.z;

        // 计算均值
        mean_acc += ( cur_acc - mean_acc ) / N;
        mean_gyr += ( cur_gyr - mean_gyr ) / N;

        // 计算协方差
        cov_acc = cov_acc * ( N - 1.0 ) / N + ( cur_acc - mean_acc ).cwiseProduct( cur_acc - mean_acc ) * ( N - 1.0 ) / ( N * N );
        cov_gyr = cov_gyr * ( N - 1.0 ) / N + ( cur_gyr - mean_gyr ).cwiseProduct( cur_gyr - mean_gyr ) * ( N - 1.0 ) / ( N * N );
        // cov_acc = Eigen::Vector3d(0.1, 0.1, 0.1);
        // cov_gyr = Eigen::Vector3d(0.01, 0.01, 0.01);
        N++;
    }

    // TODO: fix the cov
    // 这里又重新设置了两个协方差
    cov_acc = Eigen::Vector3d( COV_START_ACC_DIAG, COV_START_ACC_DIAG, COV_START_ACC_DIAG );
    cov_gyr = Eigen::Vector3d( COV_START_GYRO_DIAG, COV_START_GYRO_DIAG, COV_START_GYRO_DIAG );
    // 设置初始值
    state_inout.gravity = Eigen::Vector3d( 0, 0, 9.805 );
    state_inout.rot_end = Eye3d;
    state_inout.bias_g = mean_gyr;
}

void ImuProcess::lic_state_propagate( const MeasureGroup &meas, StatesGroup &state_inout )
{
    /*** add the imu of the last frame-tail to the of current frame-head ***/
    auto v_imu = meas.imu;
    v_imu.push_front( last_imu_ );
    // const double &imu_beg_time = v_imu.front()->header.stamp.toSec();
    const double &imu_end_time = v_imu.back()->header.stamp.toSec();
    const double &pcl_beg_time = meas.lidar_beg_time;

    /*** sort point clouds by offset time ***/
    PointCloudXYZINormal pcl_out = *( meas.lidar );
    std::sort( pcl_out.points.begin(), pcl_out.points.end(), time_list );
    // 获取扫描结束时刻时间戳
    const double &pcl_end_time = pcl_beg_time + pcl_out.points.back().curvature / double( 1000 );
    // syncpack()的时候确保了 最后一个imu时间戳一定会在激光扫描结束时刻之前
    double        end_pose_dt = pcl_end_time - imu_end_time;

    //
    state_inout = imu_preintegration( state_inout, v_imu, end_pose_dt );
    // 保存最后一帧imu，用于下一次的时候push_front
    last_imu_ = meas.imu.back();
}

// Avoid abnormal state input
bool check_state( StatesGroup &state_inout )
{
    bool is_fail = false;
    for ( int idx = 0; idx < 3; idx++ )
    {
        if ( fabs( state_inout.vel_end( idx ) ) > 10 )
        {
            is_fail = true;
            scope_color( ANSI_COLOR_RED_BG );
            for ( int i = 0; i < 10; i++ )
            {
                cout << __FILE__ << ", " << __LINE__ << ", check_state fail !!!! " << state_inout.vel_end.transpose() << endl;
            }
            state_inout.vel_end( idx ) = 0.0;
        }
    }
    return is_fail;
}

// Avoid abnormal state input
void check_in_out_state( const StatesGroup &state_in, StatesGroup &state_inout )
{
    if ( ( state_in.pos_end - state_inout.pos_end ).norm() > 1.0 )
    {
        scope_color( ANSI_COLOR_RED_BG );
        for ( int i = 0; i < 10; i++ )
        {
            cout << __FILE__ << ", " << __LINE__ << ", check_in_out_state fail !!!! " << state_in.pos_end.transpose() << " | "
                 << state_inout.pos_end.transpose() << endl;
        }
        state_inout.pos_end = state_in.pos_end;
    }
}

std::mutex g_imu_premutex;

StatesGroup ImuProcess::imu_preintegration( const StatesGroup &state_in, std::deque< sensor_msgs::Imu::ConstPtr > &v_imu, double end_pose_dt )
{
    // 为啥要加锁?
    std::unique_lock< std::mutex > lock( g_imu_premutex );
    StatesGroup                    state_inout = state_in;
    // 检查速度
    if ( check_state( state_inout ) )
    {
        state_inout.display( state_inout, "state_inout" );
        state_in.display( state_in, "state_in" );
    }
    Eigen::Vector3d acc_imu( 0, 0, 0 ), angvel_avr( 0, 0, 0 ), acc_avr( 0, 0, 0 ), vel_imu( 0, 0, 0 ), pos_imu( 0, 0, 0 );
    vel_imu = state_inout.vel_end;
    pos_imu = state_inout.pos_end;
    Eigen::Matrix3d R_imu( state_inout.rot_end );
    Eigen::MatrixXd F_x( Eigen::Matrix< double, DIM_OF_STATES, DIM_OF_STATES >::Identity() );
    Eigen::MatrixXd cov_w( Eigen::Matrix< double, DIM_OF_STATES, DIM_OF_STATES >::Zero() );
    double          dt = 0;
    int             if_first_imu = 1;
    // printf("IMU start_time = %.5f, end_time = %.5f, state_update_time = %.5f, start_delta = %.5f\r\n", v_imu.front()->header.stamp.toSec() -
    // g_lidar_star_tim,
    //        v_imu.back()->header.stamp.toSec() - g_lidar_star_tim,
    //        state_in.last_update_time - g_lidar_star_tim,
    //        state_in.last_update_time - v_imu.front()->header.stamp.toSec());
    // 遍历Imu,不包括最后一帧imu数据
    for ( std::deque< sensor_msgs::Imu::ConstPtr >::iterator it_imu = v_imu.begin(); it_imu != ( v_imu.end() - 1 ); it_imu++ )
    {
        // if(g_lidar_star_tim == 0 || state_inout.last_update_time == 0)
        // {
        //   return state_inout;
        // }
        // 取两帧imu
        sensor_msgs::Imu::ConstPtr head = *( it_imu );
        sensor_msgs::Imu::ConstPtr tail = *( it_imu + 1 );

        // 求平均，然后减去bias
        angvel_avr << 0.5 * ( head->angular_velocity.x + tail->angular_velocity.x ), 0.5 * ( head->angular_velocity.y + tail->angular_velocity.y ),
            0.5 * ( head->angular_velocity.z + tail->angular_velocity.z );
        acc_avr << 0.5 * ( head->linear_acceleration.x + tail->linear_acceleration.x ),
            0.5 * ( head->linear_acceleration.y + tail->linear_acceleration.y ), 0.5 * ( head->linear_acceleration.z + tail->linear_acceleration.z );

        angvel_avr -= state_inout.bias_g;

        acc_avr = acc_avr - state_inout.bias_a;

        // 检查时间，如果位于一帧的imu时间小于 上一次优化状态的时间，则表示imu数据无效
        if ( tail->header.stamp.toSec() < state_inout.last_update_time )
        {
            continue;
        }

        // 计算前向积分时间dt
        if ( if_first_imu )
        {
            if_first_imu = 0;
            dt = tail->header.stamp.toSec() - state_inout.last_update_time;
        }
        else
        {
            dt = tail->header.stamp.toSec() - head->header.stamp.toSec();
        }
        if ( dt > 0.05 )
        {
            dt = 0.05;
        }

        /* covariance propagation */
        Eigen::Matrix3d acc_avr_skew;
        Eigen::Matrix3d Exp_f = Exp( angvel_avr, dt );
        acc_avr_skew << SKEW_SYM_MATRIX( acc_avr );
        // Eigen::Matrix3d Jr_omega_dt = right_jacobian_of_rotion_matrix<double>(angvel_avr*dt);
        Eigen::Matrix3d Jr_omega_dt = Eigen::Matrix3d::Identity();
        F_x.block< 3, 3 >( 0, 0 ) = Exp_f.transpose();
        // F_x.block<3, 3>(0, 9) = -Eye3d * dt;
        F_x.block< 3, 3 >( 0, 9 ) = -Jr_omega_dt * dt;
        // F_x.block<3,3>(3,0)  = -R_imu * off_vel_skew * dt;
        F_x.block< 3, 3 >( 3, 3 ) = Eye3d; // Already the identity.
        F_x.block< 3, 3 >( 3, 6 ) = Eye3d * dt;
        F_x.block< 3, 3 >( 6, 0 ) = -R_imu * acc_avr_skew * dt;
        F_x.block< 3, 3 >( 6, 12 ) = -R_imu * dt;
        F_x.block< 3, 3 >( 6, 15 ) = Eye3d * dt;

        Eigen::Matrix3d cov_acc_diag, cov_gyr_diag, cov_omega_diag;
        cov_omega_diag = Eigen::Vector3d( COV_OMEGA_NOISE_DIAG, COV_OMEGA_NOISE_DIAG, COV_OMEGA_NOISE_DIAG ).asDiagonal();
        cov_acc_diag = Eigen::Vector3d( COV_ACC_NOISE_DIAG, COV_ACC_NOISE_DIAG, COV_ACC_NOISE_DIAG ).asDiagonal();
        cov_gyr_diag = Eigen::Vector3d( COV_GYRO_NOISE_DIAG, COV_GYRO_NOISE_DIAG, COV_GYRO_NOISE_DIAG ).asDiagonal();
        // cov_w.block<3, 3>(0, 0) = cov_omega_diag * dt * dt;
        cov_w.block< 3, 3 >( 0, 0 ) = Jr_omega_dt * cov_omega_diag * Jr_omega_dt * dt * dt;
        cov_w.block< 3, 3 >( 3, 3 ) = R_imu * cov_gyr_diag * R_imu.transpose() * dt * dt;
        cov_w.block< 3, 3 >( 6, 6 ) = cov_acc_diag * dt * dt;
        cov_w.block< 3, 3 >( 9, 9 ).diagonal() =
            Eigen::Vector3d( COV_BIAS_GYRO_NOISE_DIAG, COV_BIAS_GYRO_NOISE_DIAG, COV_BIAS_GYRO_NOISE_DIAG ) * dt * dt; // bias gyro covariance
        cov_w.block< 3, 3 >( 12, 12 ).diagonal() =
            Eigen::Vector3d( COV_BIAS_ACC_NOISE_DIAG, COV_BIAS_ACC_NOISE_DIAG, COV_BIAS_ACC_NOISE_DIAG ) * dt * dt; // bias acc covariance

        // cov_w.block<3, 3>(18, 18).diagonal() = Eigen::Vector3d(COV_NOISE_EXT_I2C_R, COV_NOISE_EXT_I2C_R, COV_NOISE_EXT_I2C_R) * dt * dt; // bias
        // gyro covariance cov_w.block<3, 3>(21, 21).diagonal() = Eigen::Vector3d(COV_NOISE_EXT_I2C_T, COV_NOISE_EXT_I2C_T, COV_NOISE_EXT_I2C_T) * dt
        // * dt;  // bias acc covariance cov_w(24, 24) = COV_NOISE_EXT_I2C_Td * dt * dt;

        state_inout.cov = F_x * state_inout.cov * F_x.transpose() + cov_w;

        // 状态更新
        R_imu = R_imu * Exp_f;                                              //姿态
        acc_imu = R_imu * acc_avr - state_inout.gravity;                    //加速度
        pos_imu = pos_imu + vel_imu * dt + 0.5 * acc_imu * dt * dt;         //位置
        vel_imu = vel_imu + acc_imu * dt;                                   //速度
        angvel_last = angvel_avr;
        acc_s_last = acc_imu;

        // cout <<  std::setprecision(3) << " dt = " << dt << ", acc: " << acc_avr.transpose()
        //      << " acc_imu: " << acc_imu.transpose()
        //      << " vel_imu: " << vel_imu.transpose()
        //      << " omega: " << angvel_avr.transpose()
        //      << " pos_imu: " << pos_imu.transpose()
        //       << endl;
        // cout << "Acc_avr: " << acc_avr.transpose() << endl;
    }

    // cout <<__FILE__ << ", " << __LINE__ <<" ,diagnose lio_state = " << std::setprecision(2) <<(state_inout - StatesGroup()).transpose() << endl;
    /*** calculated the pos and attitude prediction at the frame-end ***/
    // 继续推算，直到 激光扫描结束时刻
    dt = end_pose_dt;

    state_inout.last_update_time = v_imu.back()->header.stamp.toSec() + dt;
    // cout << "Last update time = " <<  state_inout.last_update_time - g_lidar_star_tim << endl;
    if ( dt > 0.1 )
    {
        scope_color( ANSI_COLOR_RED_BOLD );
        for ( int i = 0; i < 1; i++ )
        {
            cout << __FILE__ << ", " << __LINE__ << "dt = " << dt << endl;
        }
        dt = 0.1;
    }
    // 将状态保存给state_inout
    state_inout.vel_end = vel_imu + acc_imu * dt;
    state_inout.rot_end = R_imu * Exp( angvel_avr, dt );
    state_inout.pos_end = pos_imu + vel_imu * dt + 0.5 * acc_imu * dt * dt;

    // cout <<__FILE__ << ", " << __LINE__ <<" ,diagnose lio_state = " << std::setprecision(2) <<(state_inout - StatesGroup()).transpose() << endl;

    // cout << "Preintegration State diff = " << std::setprecision(2) << (state_inout - state_in).head<15>().transpose()
    //      <<  endl;
    // std::cout << __FILE__ << " " << __LINE__ << std::endl;
    // check_state(state_inout);
    if ( 0 )
    {
        if ( check_state( state_inout ) )
        {
            // printf_line;
            std::cout << __FILE__ << " " << __LINE__ << std::endl;
            state_inout.display( state_inout, "state_inout" );
            state_in.display( state_in, "state_in" );
        }
        check_in_out_state( state_in, state_inout );
    }
    // cout << (state_inout - state_in).transpose() << endl;
    return state_inout;
}

/**
 * @brief ImuProcess::lic_point_cloud_undistort
 * 将激光点投影到该帧扫描结束时刻
 * @param meas
 * @param _state_inout
 * @param pcl_out
 */
void ImuProcess::lic_point_cloud_undistort( const MeasureGroup &meas, const StatesGroup &_state_inout, PointCloudXYZINormal &pcl_out )
{
    StatesGroup state_inout = _state_inout;
    auto        v_imu = meas.imu;
    v_imu.push_front( last_imu_ );
    const double &imu_end_time = v_imu.back()->header.stamp.toSec();
    const double &pcl_beg_time = meas.lidar_beg_time;
    /*** sort point clouds by offset time ***/
    // 根据点云中的每个点的时间进行排序 (小到大)
    pcl_out = *( meas.lidar );
    std::sort( pcl_out.points.begin(), pcl_out.points.end(), time_list );
    // 计算扫描结束时间
    const double &pcl_end_time = pcl_beg_time + pcl_out.points.back().curvature / double( 1000 );
    /*std::cout << "[ IMU Process ]: Process lidar from " << pcl_beg_time - g_lidar_star_tim << " to " << pcl_end_time- g_lidar_star_tim << ", "
              << meas.imu.size() << " imu msgs from " << imu_beg_time- g_lidar_star_tim << " to " << imu_end_time- g_lidar_star_tim
              << ", last tim: " << state_inout.last_update_time- g_lidar_star_tim << std::endl;
    */
    /*** Initialize IMU pose ***/
    IMU_pose.clear();
    // IMUpose.push_back(set_pose6d(0.0, Zero3d, Zero3d, state.vel_end, state.pos_end, state.rot_end));
    // acc_s_last: 前一帧imu加速度
    // acc_s_last: 前一帧imu角速度
    // state_inout: 当前状态
    IMU_pose.push_back( set_pose6d( 0.0, acc_s_last, angvel_last, state_inout.vel_end, state_inout.pos_end, state_inout.rot_end ) );

    /*** forward propagation at each imu point ***/
    Eigen::Vector3d acc_imu, angvel_avr, acc_avr, vel_imu( state_inout.vel_end ), pos_imu( state_inout.pos_end );
    Eigen::Matrix3d R_imu( state_inout.rot_end );
    Eigen::MatrixXd F_x( Eigen::Matrix< double, DIM_OF_STATES, DIM_OF_STATES >::Identity() );
    Eigen::MatrixXd cov_w( Eigen::Matrix< double, DIM_OF_STATES, DIM_OF_STATES >::Zero() );
    double          dt = 0;
    // 遍历imu数据，不包括最后一个数据
    for ( auto it_imu = v_imu.begin(); it_imu != ( v_imu.end() - 1 ); it_imu++ )
    {
        // 取当前数据和后一帧数据
        auto &&head = *( it_imu );
        auto &&tail = *( it_imu + 1 );

        // 得到平均角速度、加速度
        angvel_avr << 0.5 * ( head->angular_velocity.x + tail->angular_velocity.x ), 0.5 * ( head->angular_velocity.y + tail->angular_velocity.y ),
            0.5 * ( head->angular_velocity.z + tail->angular_velocity.z );
        acc_avr << 0.5 * ( head->linear_acceleration.x + tail->linear_acceleration.x ),
            0.5 * ( head->linear_acceleration.y + tail->linear_acceleration.y ), 0.5 * ( head->linear_acceleration.z + tail->linear_acceleration.z );

        // 减去bias
        angvel_avr -= state_inout.bias_g;
        acc_avr = acc_avr - state_inout.bias_a;

#ifdef DEBUG_PRINT
// fout<<head->header.stamp.toSec()<<" "<<angvel_avr.transpose()<<" "<<acc_avr.transpose()<<std::endl;
#endif
        dt = tail->header.stamp.toSec() - head->header.stamp.toSec();
        /* covariance propagation */

        Eigen::Matrix3d acc_avr_skew;
        // 旋转矢量转旋转矩阵
        Eigen::Matrix3d Exp_f = Exp( angvel_avr, dt );
        acc_avr_skew << SKEW_SYM_MATRIX( acc_avr );
#ifdef DEBUG_PRINT
// fout<<head->header.stamp.toSec()<<" "<<angvel_avr.transpose()<<" "<<acc_avr.transpose()<<std::endl;
#endif

        /* propagation of IMU attitude */
        R_imu = R_imu * Exp_f;  // 姿态更新

        /* Specific acceleration (global frame) of IMU */
        acc_imu = R_imu * acc_avr - state_inout.gravity;    // 得到导航坐标系下的加速度

        /* propagation of IMU */
        pos_imu = pos_imu + vel_imu * dt + 0.5 * acc_imu * dt * dt; // 位置

        /* velocity of IMU */
        vel_imu = vel_imu + acc_imu * dt;   // 速度

        /* save the poses at each IMU measurements */
        // 保存当前加速度和角速度
        angvel_last = angvel_avr;
        acc_s_last = acc_imu;
        // 计算 后一帧imu数据时间戳 - 点云扫描起始时间戳
        double &&offs_t = tail->header.stamp.toSec() - pcl_beg_time;
        // std::cout<<"acc "<<acc_imu.transpose()<<"vel "<<acc_imu.transpose()<<"vel "<<pos_imu.transpose()<<std::endl;
        // 保存每个imu积分后的状态
        IMU_pose.push_back( set_pose6d( offs_t, acc_imu, angvel_avr, vel_imu, pos_imu, R_imu ) );
    }

    /*** calculated the pos and attitude prediction at the frame-end ***/
    dt = pcl_end_time - imu_end_time;   // 扫描结束时刻 - 最后一个imu时间戳 , 这里必须 pcl_end_time > imu_end_time ?
    // 计算在pcl_end_time时刻下的状态
    state_inout.vel_end = vel_imu + acc_imu * dt;
    state_inout.rot_end = R_imu * Exp( angvel_avr, dt );
    state_inout.pos_end = pos_imu + vel_imu * dt + 0.5 * acc_imu * dt * dt;

    // Lidar_offset_to_IMU: 激光雷达在IMU坐标系的坐标
    // pos_liD_e: 激光雷达在世界坐标系坐标
    Eigen::Vector3d pos_liD_e = state_inout.pos_end + state_inout.rot_end * Lidar_offset_to_IMU;
    // auto R_liD_e   = state_inout.rot_end * Lidar_R_to_IMU;

#ifdef DEBUG_PRINT
    std::cout << "[ IMU Process ]: vel " << state_inout.vel_end.transpose() << " pos " << state_inout.pos_end.transpose() << " ba"
              << state_inout.bias_a.transpose() << " bg " << state_inout.bias_g.transpose() << std::endl;
    std::cout << "propagated cov: " << state_inout.cov.diagonal().transpose() << std::endl;
#endif

    /*** undistort each lidar point (backward propagation) ***/
    auto it_pcl = pcl_out.points.end() - 1; // 从点云最后一个点开始
    // 从IMU_pose最后一个数据开始
    for ( auto it_kp = IMU_pose.end() - 1; it_kp != IMU_pose.begin(); it_kp-- )
    {
        // 取当前IMU_pose的前一个？？
        auto head = it_kp - 1;
        R_imu << MAT_FROM_ARRAY( head->rot );
        acc_imu << VEC_FROM_ARRAY( head->acc );
        // std::cout<<"head imu acc: "<<acc_imu.transpose()<<std::endl;
        vel_imu << VEC_FROM_ARRAY( head->vel );
        pos_imu << VEC_FROM_ARRAY( head->pos );
        angvel_avr << VEC_FROM_ARRAY( head->gyr );

        // 遍历点云
        // 已经使用的imu数据时间戳 - 点云扫描起始时间戳
        // it_pcl->curvature / double( 1000 ) 相对于扫描起始时刻的时间偏移
        // 如果当前遍历点的时间 > 这个IMU_pose的时间，则使用 [这个IMU_pose的时间，扫描结束时刻]这一段的数据进行去畸变
        for ( ; it_pcl->curvature / double( 1000 ) > head->offset_time; it_pcl-- )
        {
            // 计算 当前遍历点的时间 - 这个IMU_pose的时间
            dt = it_pcl->curvature / double( 1000 ) - head->offset_time;

            /* Transform to the 'end' frame, using only the rotation
             * Note: Compensation direction is INVERSE of Frame's moving direction
             * So if we want to compensate a point at timestamp-i to the frame-e
             * P_compensate = R_imu_e ^ T * (R_i * P_i + T_ei) where T_ei is represented in global frame */
            // 向前积分，得到当前遍历点的时间 对应的 状态
            Eigen::Matrix3d R_i( R_imu * Exp( angvel_avr, dt ) );
            // pos_liD_e: 扫描结束时刻，激光雷达在世界坐标系坐标
            // pos_imu + vel_imu * dt + 0.5 * acc_imu * dt * dt + R_i * Lidar_offset_to_IMU: 当前遍历点的时间 对应的激光雷达在世界坐标系坐标
            // T_ei:
            Eigen::Vector3d T_ei( pos_imu + vel_imu * dt + 0.5 * acc_imu * dt * dt + R_i * Lidar_offset_to_IMU - pos_liD_e );

            Eigen::Vector3d P_i( it_pcl->x, it_pcl->y, it_pcl->z );
            // 由于出厂时Livox的激光坐标系和IMU坐标系可以认为旋转很小，这里直接使用IMU姿态R_i来代替激光的姿态 (ATTENTION!)
            Eigen::Vector3d P_compensate = state_inout.rot_end.transpose() * ( R_i * P_i + T_ei );

            // P_compensate: 当前遍历点 投影到 扫描结束时刻下，该点在激光雷达坐标系的坐标
            // state_inout.rot_end * P_compensate = R_i * P_i + T_ei
            // 下面等号两侧均表示： 当前遍历点 投影到 扫描结束时刻下, 该点在世界坐标系的坐标
            // state_inout.rot_end * P_compensate + pos_liD_e = R_i * P_i + (pos_imu + vel_imu * dt + 0.5 * acc_imu * dt * dt + R_i * Lidar_offset_to_IMU)

            /// save Undistorted points and their rotation
            it_pcl->x = P_compensate( 0 );
            it_pcl->y = P_compensate( 1 );
            it_pcl->z = P_compensate( 2 );

            if ( it_pcl == pcl_out.points.begin() )
                break;
        }
    }
}

void ImuProcess::Process( const MeasureGroup &meas, StatesGroup &stat, PointCloudXYZINormal::Ptr cur_pcl_un_ )
{
    // double t1, t2, t3;
    // t1 = omp_get_wtime();

    if ( meas.imu.empty() )
    {
        // std::cout << "no imu data" << std::endl;
        return;
    };
    ROS_ASSERT( meas.lidar != nullptr );

    // IMU初始化
    if ( imu_need_init_ )
    {
        /// The very first lidar frame
        IMU_Initial( meas, stat, init_iter_num );

        imu_need_init_ = true;

        // 最新imu
        last_imu_ = meas.imu.back();

        // imu数据量>20 ， 认为初始化完成
        if ( init_iter_num > MAX_INI_COUNT )
        {
            imu_need_init_ = false;
            // std::cout<<"mean acc: "<<mean_acc<<" acc measures in word frame:"<<state.rot_end.transpose()*mean_acc<<std::endl;
            ROS_INFO(
                "IMU Initials: Gravity: %.4f %.4f %.4f; state.bias_g: %.4f %.4f %.4f; acc covarience: %.8f %.8f %.8f; gry covarience: %.8f %.8f %.8f",
                stat.gravity[ 0 ], stat.gravity[ 1 ], stat.gravity[ 2 ], stat.bias_g[ 0 ], stat.bias_g[ 1 ], stat.bias_g[ 2 ], cov_acc[ 0 ],
                cov_acc[ 1 ], cov_acc[ 2 ], cov_gyr[ 0 ], cov_gyr[ 1 ], cov_gyr[ 2 ] );
        }

        return;
    }

    /// Undistort points： the first point is assummed as the base frame
    /// Compensate lidar points with IMU rotation (with only rotation now)
    // if ( 0 || (stat.last_update_time < 0.1))

    if ( 0 )
    {
        // UndistortPcl(meas, stat, *cur_pcl_un_);
    }
    else
    {
        if ( 1 )
        {
            // 畸变矫正, 将激光点投影到该帧扫描结束时刻
            lic_point_cloud_undistort( meas, stat, *cur_pcl_un_ );
        }
        else
        {
            *cur_pcl_un_ = *meas.lidar;
        }
        lic_state_propagate( meas, stat );
    }
    // t2 = omp_get_wtime();

    // 保存上一次处理的最后一帧imu
    last_imu_ = meas.imu.back();

    // t3 = omp_get_wtime();

    // std::cout<<"[ IMU Process ]: Time: "<<t3 - t1<<std::endl;
}
