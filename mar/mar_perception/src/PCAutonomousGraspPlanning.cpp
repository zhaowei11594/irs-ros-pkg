/*
 * PCGraspPlanning.cpp
 *
 *  Created on: 07/06/2012
 *      Author: dfornas
 *      Modified 23/01/2013 by dfornas
 */

#include <ros/ros.h>

//#include <mar_perception/VispUtils.h>
#include <mar_perception/PCAutonomousGraspPlanning.h>

#include <visp/vpPixelMeterConversion.h>
#include <visp/vpHomogeneousMatrix.h>
#include <visp/vpPoint.h>

//Includes del reconstruction, algunos pueden sobrar...
#include <pcl/ModelCoefficients.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>

//Comprobar si son necesarias
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/conditional_removal.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/surface/convex_hull.h>

#include <tf/transform_datatypes.h>
#include <boost/bind.hpp>

#include <vector>
#include <algorithm>

void PCAutonomousGraspPlanning::perceive() {

  // Datasets
  pcl::PointCloud<PointT>::Ptr cloud_filtered (new pcl::PointCloud<PointT>);
  pcl::PointCloud<pcl::Normal>::Ptr cloud_normals (new pcl::PointCloud<pcl::Normal>);
  pcl::PointCloud<PointT>::Ptr cloud_filtered2 (new pcl::PointCloud<PointT>);
  pcl::PointCloud<pcl::Normal>::Ptr cloud_normals2 (new pcl::PointCloud<pcl::Normal>);

  pcl::ModelCoefficients::Ptr coefficients_plane (new pcl::ModelCoefficients), coefficients_cylinder (new pcl::ModelCoefficients);
  //Nubes del plano y cilindro.
  pcl::PointCloud<PointT>::Ptr cloud_cylinder (new pcl::PointCloud<PointT> ());
  pcl::PointCloud<PointT>::Ptr cloud_plane (new pcl::PointCloud<PointT> ());

  PCLUtils::passThrough(cloud_, cloud_filtered, -10, 10);
  std::cerr << "PointCloud after filtering has: " << cloud_filtered->points.size () << " data points." << std::endl;

  // @todo : Add more filters -> downsampling and radial ooutlier removal.
  PCLUtils::estimateNormals(cloud_filtered, cloud_normals);
    
  coefficients_plane = PCLUtils::planeSegmentation(cloud_filtered, cloud_normals, cloud_filtered2, cloud_normals2, cloud_plane, plane_distance_threshold_, plane_iterations_);
  coefficients_cylinder = PCLUtils::cylinderSegmentation(cloud_filtered2, cloud_normals2, cloud_cylinder, cylinder_distance_threshold_, cylinder_iterations_, radious_limit_);
  PCLUtils::showClouds(cloud_plane, cloud_cylinder, coefficients_plane, coefficients_cylinder);


  //Puntos de agarre
  PointT mean, max, min;  //Puntos que definiran el cilindro.
  
  //Punto de origen del cilindro.
  PointT axis_point;
  axis_point.x=coefficients_cylinder->values[0];
  axis_point.y=coefficients_cylinder->values[1];
  axis_point.z=coefficients_cylinder->values[2];
  
  //Vectores directores
  tf::Vector3 axis_dir(coefficients_cylinder->values[3], coefficients_cylinder->values[4], coefficients_cylinder->values[5]);
  axis_dir=axis_dir.normalize();
  //Vector perpendicular a la dirección, en el plano Z de la cámara (ejesX,Y)
  tf::Vector3 perp(coefficients_cylinder->values[4], -coefficients_cylinder->values[3], 0);
  perp=perp.normalize();
  //Vector ortogonal correspondiente
  tf::Vector3 result=tf::tfCross( perp, axis_dir).normalize();
 
  getMinMax3DAlongAxis(cloud_cylinder, &max, &min, axis_point, &axis_dir);
  //Punto medio reposicionado.
  mean.x=(max.x+min.x)/2;mean.y=(max.y+min.y)/2;mean.z=(max.z+min.z)/2;
 coefficients_cylinder->values[0]=mean.x;
   coefficients_cylinder->values[1]=mean.y;
  coefficients_cylinder->values[2]=mean.z;
  radious=coefficients_cylinder->values[6];
  height=sqrt((max.x-min.x)*(max.x-min.x)+(max.y-min.y)*(max.y-min.y)+(max.z-min.z)*(max.z-min.z));

  //DEBUG, check if they are ortogonal.
  //std::cout << "Mean -> x: " << mean.x << " y: " << mean.y << " z: " << mean.z << std::endl;
  //std::cout << "Max -> x: " << max.x << " y: " << max.y << " z: " << max.z << std::endl;
  //std::cout << "Min -> x: " << min.x << " y: " << min.y << " z: " << min.z << std::endl;
  //std::cout << "AxisDir -> x: " << axis_dir.x() << " y: " << axis_dir.y() << " z: " << axis_dir.z() << std::endl;
  //std::cout << "perp -> x: " << perp.x() << " y: " <<perp.y() << " z: " <<perp.z() << std::endl;
  //std::cout << "result -> x: " << result.x() << " y: " << result.y() << " z: " << result.z() << std::endl;//OK UNIT
  //std::cout << "Dot products" << result.dot(perp) << result.dot(axis_dir) << axis_dir.dot(perp);//OK ZEROS

  //Ahora mismo el end-efector cae dentro del cilindro en vez de en superfície.
  //Esto está relativamente bien pero no tenemos en cuenta la penetración. Sin emargo, la 
  //tenemos en cuenta luego al separanos el radio así que no hay problema en realidad. 
  cMo[0][0]=perp.x(); cMo[0][1]=axis_dir.x(); cMo[0][2]=result.x();cMo[0][3]=mean.x;
  cMo[1][0]=perp.y(); cMo[1][1]=axis_dir.y(); cMo[1][2]=result.y();cMo[1][3]=mean.y;
  cMo[2][0]=perp.z(); cMo[2][1]=axis_dir.z(); cMo[2][2]=result.z();cMo[2][3]=mean.z;
  cMo[3][0]=0;cMo[3][1]=0;cMo[3][2]=0;cMo[3][3]=1;
  //ROS_INFO_STREAM("cMo is...: " << std::endl << cMo << "Is homog: " << cMo.isAnHomogeneousMatrix()?"yes":"no");
  vispToTF.resetTransform(cMo, "1");
  
  //DEBUG Print MAX and MIN frames
  /*vpHomogeneousMatrix cMg2(cMo);
  cMg2[0][3]=max.x;
  cMg2[1][3]=max.y;
  cMg2[2][3]=max.z;
  vispToTF.addTransform(cMg2, "/stereo", "/max", "3");
  cMg2[0][3]=min.x;
  cMg2[1][3]=min.y;
  cMg2[2][3]=min.z;
  vispToTF.addTransform(cMg2, "/stereo", "/min", "4");  */
  
  //Compute modified cMg from cMo
  recalculate_cMg();
}

//Compute cMg from cMo
void PCAutonomousGraspPlanning::recalculate_cMg(){

  //Set grasp config values from interface int values.
  intToConfig();
  vpHomogeneousMatrix oMg;
  
  if(aligned_grasp_){
    //Aplicamos una rotación y traslación para posicionar la garra mejor
    vpHomogeneousMatrix grMgt0(0,along_,0,0,0,0);
    vpHomogeneousMatrix gMgrZ(0,0,0,0,0,1.57);
    vpHomogeneousMatrix gMgrX(0,0,0,1.57,0,0);		
    vpHomogeneousMatrix gMgrY(0,0,0,0,0,angle_);		
    vpHomogeneousMatrix grMgt(rad_,0,0,0,0,0);
    oMg = grMgt0 * gMgrZ * gMgrX * gMgrY * grMgt;
    cMg = cMo * oMg ;
  }else{
    //aplicamos una rotación y traslación para posicionar la garra mejor
    vpHomogeneousMatrix gMgr(0,0,0,0,angle_,0);
    vpHomogeneousMatrix grMgt0(rad_,0,0,0,0,0);
    vpHomogeneousMatrix grMgt1(0,along_,0,0,0,0);
    oMg = gMgr * grMgt0 * grMgt1;
    cMg = cMo  * oMg ;
  }
  vpHomogeneousMatrix rot(0,0,0,0,1.57,0);
  cMg=cMg*rot;
  
  //Compute bMg and plan a grasp on bMg
  //vpHomogeneousMatrix bMg=bMc*cMg;
  //std::cerr << "bMg is: " << std::endl << bMg << std::endl;
  
  //Publish cMg in TF, 2 is the TF list index.
  vispToTF.resetTransform(cMg, "2");
  vispToTF.publish();
}

/// Set config values: from int sliders to float values.
void PCAutonomousGraspPlanning::intToConfig(){
  bool old=aligned_grasp_;
  aligned_grasp_=ialigned_grasp==1?true:false;

  // @todo: Move defaults to other place.
  if(old!=aligned_grasp_){
    if(aligned_grasp_){iangle=45;irad=48;ialong=31;}
    else{ iangle=226;irad=50;ialong=20;}
  }

  angle_=iangle*(2.0*M_PI/360.0);
  rad_=-irad/100.0;
  along_=(ialong-20)/100.0;//to allow 20 cm negative.... should allow a range based on minMax distance
}

///Ordenar en función de la proyección del punto sobre el eje definido 
///por axis_point_g y normal_g (globales)
bool PCAutonomousGraspPlanning::sortFunction(const PointT& d1, const PointT& d2)
{
  double t1 = (normal_g.x()*(d1.x-axis_point_g.x) + normal_g.y()*(d1.y-axis_point_g.y) + normal_g.z()*(d1.z-axis_point_g.z))/(pow(normal_g.x(),2) + pow(normal_g.y(),2) + pow(normal_g.z(),2));
  double t2 = (normal_g.x()*(d2.x-axis_point_g.x) + normal_g.y()*(d2.y-axis_point_g.y) + normal_g.z()*(d2.z-axis_point_g.z))/(pow(normal_g.x(),2) + pow(normal_g.y(),2) + pow(normal_g.z(),2));

  return t1 < t2;
}

///Obtiene los máximos y mínimos del cilindro para encontrar la altura del cilindro con un margen
///de descarte del 5%.
void PCAutonomousGraspPlanning::getMinMax3DAlongAxis(const pcl::PointCloud<PointT>::ConstPtr& cloud, PointT * max_pt, PointT * min_pt, PointT axis_point, tf::Vector3 * normal)
{
  axis_point_g=axis_point; 
  normal_g=*normal;

  PointT max_p = axis_point;
  double max_t = 0.0;
  PointT min_p = axis_point;
  double min_t = 0.0;
  std::vector<PointT> list;
  PointT* k;

  //Al tener la lista de todos los puntos podemos descartar los que esten fuera de un 
  //determinado porcentaje (percentiles) para
  //Eliminar más outliers y ganar robustez.
  BOOST_FOREACH(const PointT& pt, cloud->points)
  {
    k=new PointT();
    k->x=pt.x*1;k->y=pt.y*1;k->z=pt.z*1;
    list.push_back(*k);
  }
  //Ordenamos con respecto al eje de direccion y tomamos P05 y P95
  std::sort(list.begin(), list.end(),  boost::bind(&PCAutonomousGraspPlanning::sortFunction, this, _1, _2));
  PointT max=list[(int)list.size()*0.05],min=list[(int)list.size()*0.95];
  //Proyección de los puntos reales a puntos sobre la normal.
  double t = (normal->x()*(max.x-axis_point.x) + normal->y()*(max.y-axis_point.y) + normal->z()*(max.z-axis_point.z))/(pow(normal->x(),2) + pow(normal->y(),2) + pow(normal->z(),2));
  PointT p;
  p.x = axis_point.x + normal->x() * t;
  p.y = axis_point.y + normal->y() * t;
  p.z = axis_point.z + normal->z() * t;
  *max_pt=p;
  t = (normal->x()*(min.x-axis_point.x) + normal->y()*(min.y-axis_point.y) + normal->z()*(min.z-axis_point.z))/(pow(normal->x(),2) + pow(normal->y(),2) + pow(normal->z(),2));
  p.x = axis_point.x + normal->x() * t;
  p.y = axis_point.y + normal->y() * t;
  p.z = axis_point.z + normal->z() * t;
  *min_pt=p;
  ///@todo MAke a list with this points and show it.
  
}

/// @todo Add d,a,r as parameters.
void PCAutonomousGraspPlanning::generateGraspList()
{
  for (double d = -0.2; d <= 0.2; d += 0.04)
  {
    for (double a = 40; a <= 140; a += 8)
    {
      for (double r = -0.5; r <= -0.2; r += 0.05)
      {
        double angle = a * 2 * 3.1416 / 360; // Deg -> Rad
        vpHomogeneousMatrix grMgt0(0, d, 0, 0, 0, 0);
        vpHomogeneousMatrix gMgrZ(0, 0, 0, 0, 0, 1.57);
        vpHomogeneousMatrix gMgrX(0, 0, 0, 1.57, 0, 0);
        vpHomogeneousMatrix gMgrY(0, 0, 0, 0, 0, angle);
        vpHomogeneousMatrix grMgt(r, 0, 0, 0, 0, 0);
        vpHomogeneousMatrix oMg = grMgt0 * gMgrZ * gMgrX * gMgrY * grMgt;
        cMg = cMo * oMg;
        vpHomogeneousMatrix rot(0, 0, 0, 0, 1.57, 0);
        cMg=cMg*rot;
        cMg_list.push_back(cMg);
      }
    }
  }
}

//As a kinematic filter will be applied, I prefer to do it externally to avoid adding more deps.
void PCAutonomousGraspPlanning::filterGraspList(){

  std::list<vpHomogeneousMatrix>::iterator i = cMg_list.begin();
  while (i != cMg_list.end())
  {
      if (!false)//Replace with desired condition
      {
        cMg_list.erase(i++);  // alternatively, i = items.erase(i);
      }
  }
}


