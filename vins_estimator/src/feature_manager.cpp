#include "feature_manager.h"
//https://blog.csdn.net/try_again_later/article/details/104893694

//返回最后一个观测到这个特征点的图像帧ID
int FeaturePerId::endFrame()
{
    return start_frame + feature_per_frame.size() - 1; //feature_per_frame的数量为vector容器中FeaturePerFrame基本类的数量。代表能够观测到某个特征点的所有帧
}

FeatureManager::FeatureManager(Matrix3d _Rs[])
    : Rs(_Rs)
{
    for (int i = 0; i < NUM_OF_CAM; i++)
        ric[i].setIdentity();
}

void FeatureManager::setRic(Matrix3d _ric[])
{
    for (int i = 0; i < NUM_OF_CAM; i++)
    {
        ric[i] = _ric[i];
    }
}

void FeatureManager::clearState()
{
    feature.clear();
}

/* 
窗口中被跟踪特征点的数量。
标准：该特征点被两帧以上观测到了，且第一次观测到的帧数不是在最后面。
*/
int FeatureManager::getFeatureCount()
{
    int cnt = 0;
    //遍历feature
    for (auto &it : feature)
    {

        //所有特征点被观测到的帧数
        it.used_num = it.feature_per_frame.size();

        //如果该特征点有两帧以上观测到了  且第一次观测到帧数不是在最后
        if (it.used_num >= 2 && it.start_frame < WINDOW_SIZE - 2)
        {
            cnt++; //这个特征点是有效的
        }
    }
    return cnt;
}

//const map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> &image   (特征点ID，相机ID，(x,y,z,u,v,vx,vy)
//把特征点从image放入feature的list容器中，计算每一个点跟踪次数小于阈值 和 它在次新帧和次次新帧间所有特征点的平均视差大于阈值，返回是否是关键帧 
bool FeatureManager::addFeatureCheckParallax(int frame_count, const map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> &image, double td)
{
    ROS_DEBUG("input feature: %d", (int)image.size()); //特征点数量
    ROS_DEBUG("num of feature: %d", getFeatureCount()); //能够作为特征点的数量
    double parallax_sum = 0; //所有特征点视差总和
    int parallax_num = 0;
    last_track_num = 0; //被跟踪的个数

    // 把image map中的所有特征点放入feature list容器中
    // 遍历特征点,看该特征点是否在特征点的列表中,如果没在,则将<FeatureID,Start_frame>存入到Feature列表中；否则统计数目
    for (auto &id_pts : image) //遍历所有特征点
    {
        FeaturePerFrame f_per_fra(id_pts.second[0].second, td); // 每帧的特征点[x,y,z,u,v,vx,vy]和td td是IMU和cam同步时间差

        //迭代器寻找feature list中是否有这feature_id
        int feature_id = id_pts.first;
        //第三个参数是 Lambda表达式
        auto it = find_if(feature.begin(), feature.end(), [feature_id](const FeaturePerId &it)
                          {
            return it.feature_id == feature_id;
                          });

        //如果没有则新建一个，并在feature管理器的list容器最后添加：FeaturePerId、FeaturePerFrame
        if (it == feature.end())
        {
            feature.push_back(FeaturePerId(feature_id, frame_count)); //（特征点ID，首次观测到特征点的图像帧ID）
            feature.back().feature_per_frame.push_back(f_per_fra); 
        }
        //之前有的话在FeaturePerFrame添加此特征点在此帧的位置和其他信息，并统计数目。
        else if (it->feature_id == feature_id)
        {
            it->feature_per_frame.push_back(f_per_fra);
            last_track_num++; //此帧有多少相同特征点被跟踪
        }
    }

    //追踪次数小于20或者窗口内帧的数目小于2，是关键帧
    if (frame_count < 2 || last_track_num < 20)
        return true;

    //计算每个特征在次新帧和次次新帧中的视差
    for (auto &it_per_id : feature)
    {
        //观测该特征点的：起始帧小于倒数第三帧，终止帧要大于倒数第二帧，保证至少有两帧能观测到。
        if (it_per_id.start_frame <= frame_count - 2 &&
            it_per_id.start_frame + int(it_per_id.feature_per_frame.size()) - 1 >= frame_count - 1)
        {
            //总视差：该特征点在两帧的归一化平面上的坐标点的距离ans
            parallax_sum += compensatedParallax2(it_per_id, frame_count);
            parallax_num++; //个数
        }
    }

    //第一次加进去的，是关键帧
    if (parallax_num == 0)
    {
        return true;
    }
    else
    {
        ROS_DEBUG("parallax_sum: %lf, parallax_num: %d", parallax_sum, parallax_num);
        ROS_DEBUG("current parallax: %lf", parallax_sum / parallax_num * FOCAL_LENGTH);
        //平均视差大于阈值的是关键帧
        return parallax_sum / parallax_num >= MIN_PARALLAX;
    }
}

void FeatureManager::debugShow()
{
    ROS_DEBUG("debug show");
    for (auto &it : feature)
    {
        ROS_ASSERT(it.feature_per_frame.size() != 0);
        ROS_ASSERT(it.start_frame >= 0);
        ROS_ASSERT(it.used_num >= 0);

        ROS_DEBUG("%d,%d,%d ", it.feature_id, it.used_num, it.start_frame);
        int sum = 0;
        for (auto &j : it.feature_per_frame)
        {
            ROS_DEBUG("%d,", int(j.is_used));
            sum += j.is_used;
            printf("(%lf,%lf) ",j.point(0), j.point(1));
        }
        ROS_ASSERT(it.used_num == sum);
    }
}

//得到给定两帧之间的对应特征点3D坐标
vector<pair<Vector3d, Vector3d>> FeatureManager::getCorresponding(int frame_count_l, int frame_count_r)
{
    vector<pair<Vector3d, Vector3d>> corres;
    for (auto &it : feature) //遍历feature的list容器
    {
        //要找特征点的两帧在窗口范围内，可以直接取。窗口为：观测到当前特征点的所有图像帧
        if (it.start_frame <= frame_count_l && it.endFrame() >= frame_count_r)
        {
            Vector3d a = Vector3d::Zero(), b = Vector3d::Zero();
            int idx_l = frame_count_l - it.start_frame; //当前帧-第一次观测到特征点的帧数
            int idx_r = frame_count_r - it.start_frame;

            a = it.feature_per_frame[idx_l].point;

            b = it.feature_per_frame[idx_r].point;
            
            corres.push_back(make_pair(a, b));
        }
    }
    return corres;
}

//设置特征点的逆深度估计值
void FeatureManager::setDepth(const VectorXd &x)
{
    int feature_index = -1;
    for (auto &it_per_id : feature)
    {
        //至少两帧观测得到这个特征点  且首次观测到该特征点的图像帧在滑动窗范围内
        it_per_id.used_num = it_per_id.feature_per_frame.size();
        if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
            continue;

        //求解逆深度
        it_per_id.estimated_depth = 1.0 / x(++feature_index);
        //ROS_INFO("feature id %d , start_frame %d, depth %f ", it_per_id->feature_id, it_per_id-> start_frame, it_per_id->estimated_depth);
        
        //深度小于0估计失败
        if (it_per_id.estimated_depth < 0)
        {
            it_per_id.solve_flag = 2; //失败估计
        }
        else
            it_per_id.solve_flag = 1; //成功估计
    }
}

//剔除feature中估计失败的点（solve_flag == 2）
void FeatureManager::removeFailures()
{
    for (auto it = feature.begin(), it_next = feature.begin();
         it != feature.end(); it = it_next)
    {
        it_next++;
        if (it->solve_flag == 2)
            feature.erase(it);
    }
}

void FeatureManager::clearDepth(const VectorXd &x)
{
    int feature_index = -1;
    for (auto &it_per_id : feature)
    {
        it_per_id.used_num = it_per_id.feature_per_frame.size();
        if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
            continue;
        it_per_id.estimated_depth = 1.0 / x(++feature_index);
    }
}

VectorXd FeatureManager::getDepthVector()
{
    VectorXd dep_vec(getFeatureCount());
    int feature_index = -1;
    for (auto &it_per_id : feature)
    {
        it_per_id.used_num = it_per_id.feature_per_frame.size();
        if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
            continue;
#if 1
        dep_vec(++feature_index) = 1. / it_per_id.estimated_depth;
#else
        dep_vec(++feature_index) = it_per_id->estimated_depth;
#endif
    }
    return dep_vec;
}

//对特征点进行三角化求深度（SVD分解）
void FeatureManager::triangulate(Vector3d Ps[], Vector3d tic[], Matrix3d ric[])
{
    for (auto &it_per_id : feature)
    {
        //需要至少两帧观测到该特征点 且 首次观测到特征点的帧不是倒数第三帧
        it_per_id.used_num = it_per_id.feature_per_frame.size();
        if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
            continue;

        if (it_per_id.estimated_depth > 0) //该id的特征点深度值大于0，该值在初始化时为-1，如果大于0，说明该点被三角化过
            continue;

        //// imu_i：观测到该特征点的第一帧图像在滑动窗口中的帧号
        //imu_j：观测到该特征点的最后一帧图像在滑动窗口中的帧号
        int imu_i = it_per_id.start_frame, imu_j = imu_i - 1;

        ROS_ASSERT(NUM_OF_CAM == 1);
        Eigen::MatrixXd svd_A(2 * it_per_id.feature_per_frame.size(), 4);
        int svd_idx = 0;

        //R0 t0为第i帧相机坐标系到世界坐标系
        Eigen::Matrix<double, 3, 4> P0;
        Eigen::Vector3d t0 = Ps[imu_i] + Rs[imu_i] * tic[0]; //Rs是imu到world系，Ps是imu在world系下的位置
        Eigen::Matrix3d R0 = Rs[imu_i] * ric[0];
        P0.leftCols<3>() = Eigen::Matrix3d::Identity(); //单位旋转矩阵
        P0.rightCols<1>() = Eigen::Vector3d::Zero(); //0平移向量

        // 对于观测到该id特征点的每一图像帧
        for (auto &it_per_frame : it_per_id.feature_per_frame)
        {
            imu_j++;

            //R1 t1为第j帧相机坐标系到世界坐标系
            Eigen::Vector3d t1 = Ps[imu_j] + Rs[imu_j] * tic[0];
            Eigen::Matrix3d R1 = Rs[imu_j] * ric[0];
            //R t为第j帧相机坐标系到第i帧相机坐标系的变换矩阵，P为i到j的变换矩阵
            Eigen::Vector3d t = R0.transpose() * (t1 - t0); //R0是i camera到world系
            Eigen::Matrix3d R = R0.transpose() * R1;
            Eigen::Matrix<double, 3, 4> P;
            P.leftCols<3>() = R.transpose();
            P.rightCols<1>() = -R.transpose() * t; //slam十四讲公式3.14
            Eigen::Vector3d f = it_per_frame.point.normalized(); //长度为1
            svd_A.row(svd_idx++) = f[0] * P.row(2) - f[2] * P.row(0); //u=f[0]/f[2],v=f[1]/f[2].  SVD分解
            svd_A.row(svd_idx++) = f[1] * P.row(2) - f[2] * P.row(1);

            if (imu_i == imu_j)
                continue;
        }

        //对A的SVD分解得到其最小奇异值对应的单位奇异向量(x,y,z,w)，深度为z/w
        ROS_ASSERT(svd_idx == svd_A.rows());
        Eigen::Vector4d svd_V = Eigen::JacobiSVD<Eigen::MatrixXd>(svd_A, Eigen::ComputeThinV).matrixV().rightCols<1>();
        double svd_method = svd_V[2] / svd_V[3];
        //it_per_id->estimated_depth = -b / A;
        //it_per_id->estimated_depth = svd_V[2] / svd_V[3];

        it_per_id.estimated_depth = svd_method; //该特征点的逆深度
        //it_per_id->estimated_depth = INIT_DEPTH;

        // 如果估计出来的深度小于0.1，则把它替换为一个设定的值
        if (it_per_id.estimated_depth < 0.1)
        {
            it_per_id.estimated_depth = INIT_DEPTH;
        }

    }
}

void FeatureManager::removeOutlier()
{
    ROS_BREAK();
    int i = -1;
    for (auto it = feature.begin(), it_next = feature.begin();
         it != feature.end(); it = it_next)
    {
        it_next++;
        i += it->used_num != 0;
        if (it->used_num != 0 && it->is_outlier == true)
        {
            feature.erase(it);
        }
    }
}

//边缘化最老帧时，处理特征点保存的帧号，将起始帧是最老帧的特征点的深度值进行转移
void FeatureManager::removeBackShiftDepth(Eigen::Matrix3d marg_R, Eigen::Vector3d marg_P, Eigen::Matrix3d new_R, Eigen::Vector3d new_P)
{
    for (auto it = feature.begin(), it_next = feature.begin();
         it != feature.end(); it = it_next)
    {
        it_next++;

        //特征点起始帧不是最老帧则将帧号减一
        if (it->start_frame != 0)
            it->start_frame--;
        else
        {
            //特征点起始帧是最老帧
            Eigen::Vector3d uv_i = it->feature_per_frame[0].point;  
            it->feature_per_frame.erase(it->feature_per_frame.begin());
            
            //特征点只在最老帧被观测则直接移除
            if (it->feature_per_frame.size() < 2)
            {
                feature.erase(it);
                continue;
            }
            else
            {
                //pts_i为特征点在最老帧坐标系下的三维坐标
                //w_pts_i为特征点在世界坐标系下的三维坐标
                //将其转换到在下一帧坐标系下的坐标pts_j
                Eigen::Vector3d pts_i = uv_i * it->estimated_depth;
                Eigen::Vector3d w_pts_i = marg_R * pts_i + marg_P;
                Eigen::Vector3d pts_j = new_R.transpose() * (w_pts_i - new_P); //new_R是帧坐标系到世界坐标系
                double dep_j = pts_j(2); //深度值
                if (dep_j > 0)
                    it->estimated_depth = dep_j;
                else
                    it->estimated_depth = INIT_DEPTH;
            }
        }
        // remove tracking-lost feature after marginalize
        /*
        if (it->endFrame() < WINDOW_SIZE - 1)
        {
            feature.erase(it);
        }
        */
    }
}

//边缘化最老帧时，直接将特征点所保存的帧号向前滑动
void FeatureManager::removeBack()
{
    for (auto it = feature.begin(), it_next = feature.begin();
         it != feature.end(); it = it_next)
    {
        it_next++;

        //如果特征点起始帧号start_frame不为零则减一
        if (it->start_frame != 0)
            it->start_frame--;
        
        //如果start_frame为0则直接移除feature_per_frame的第0帧FeaturePerFrame
        //如果feature_per_frame为空则直接删除特征点
        else
        {
            it->feature_per_frame.erase(it->feature_per_frame.begin());
            if (it->feature_per_frame.size() == 0)
                feature.erase(it);
        }
    }
}

//边缘化次新帧时，对特征点在次新帧的信息进行移除处理   frame_count 帧数，也就是最后一帧
void FeatureManager::removeFront(int frame_count)
{
    for (auto it = feature.begin(), it_next = feature.begin(); it != feature.end(); it = it_next)
    {
        it_next++;

        //起始帧为最新帧的滑动成次新帧
        if (it->start_frame == frame_count)
        {
            it->start_frame--;
        }
        else
        {
            int j = WINDOW_SIZE - 1 - it->start_frame;
            
            //如果次新帧之前已经跟踪结束则什么都不做
            if (it->endFrame() < frame_count - 1)
                continue;

            //如果在次新帧仍被跟踪，则删除feature_per_frame中次新帧对应的FeaturePerFrame
            //如果feature_per_frame为空则直接删除特征点
            it->feature_per_frame.erase(it->feature_per_frame.begin() + j);
            if (it->feature_per_frame.size() == 0)
                feature.erase(it);
        }
    }
}

//计算某个特征点it_per_id在次新帧和次次新帧的视差ans
//判断观测到该特征点的frame中倒数第二帧和倒数第三帧的共视关系 实际是求取该特征点在两帧的归一化平面上的坐标点的距离ans
double FeatureManager::compensatedParallax2(const FeaturePerId &it_per_id, int frame_count)
{
    //check the second last frame is keyframe or not
    //parallax betwwen seconde last frame and third last frame
    const FeaturePerFrame &frame_i = it_per_id.feature_per_frame[frame_count - 2 - it_per_id.start_frame];
    const FeaturePerFrame &frame_j = it_per_id.feature_per_frame[frame_count - 1 - it_per_id.start_frame];

    double ans = 0;
    Vector3d p_j = frame_j.point; //3D路标点（倒数第二帧j）

    //归一化坐标
    double u_j = p_j(0);
    double v_j = p_j(1);

    Vector3d p_i = frame_i.point; //3D路标点（倒数第三帧i）
    Vector3d p_i_comp;

    //int r_i = frame_count - 2;
    //int r_j = frame_count - 1;
    //p_i_comp = ric[camera_id_j].transpose() * Rs[r_j].transpose() * Rs[r_i] * ric[camera_id_i] * p_i;
    p_i_comp = p_i;
    double dep_i = p_i(2); //深度值

    //归一化坐标
    double u_i = p_i(0) / dep_i;
    double v_i = p_i(1) / dep_i;

    double du = u_i - u_j, dv = v_i - v_j;

    double dep_i_comp = p_i_comp(2);
    double u_i_comp = p_i_comp(0) / dep_i_comp;
    double v_i_comp = p_i_comp(1) / dep_i_comp;

    double du_comp = u_i_comp - u_j, dv_comp = v_i_comp - v_j;

    ans = max(ans, sqrt(min(du * du + dv * dv, du_comp * du_comp + dv_comp * dv_comp)));

    return ans;
}