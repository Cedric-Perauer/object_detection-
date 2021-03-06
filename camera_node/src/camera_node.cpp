#include "ros/ros.h"
#include "std_msgs/String.h"
#include <cv_bridge/cv_bridge.h>
#include <sstream>
#include <image_transport/image_transport.h>
#include <sensor_msgs/CameraInfo.h>
#include <opencv2/core.hpp> 
#include <sensor_msgs/image_encodings.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include "params.h"
#include <iostream>
#include <chrono>
#include "cuda_runtime_api.h"
#include "logging.h"
#include "common.hpp"
#include "yolov5.cpp"
#include "rektnet.cpp"
#include "torch/torch.h"
#include <sstream> 


std::string paramsFilePath = "/home/cedric/CATKIN_FS/src/02_perception/camera/camera_node/src/params.txt"; 


const float FOCAL_LENGTH = 1.2e-3; //the same for x and y 
const float CONE_HEIGHT_SMALL = 0.28; //small cone heigh to be determined, dummy for now
const float CONE_HEIGHT_LARGE = 0.4; //large cone height to be determined, dummy for now
const float PIXEL_SIZE = 5.86e-6; //size of pixels in the sensor, found from camera datasheet
const float CENTER_X = 0; //optical center X 
const float CENTER_Y = 2; //optical center Y
const float IMG_HALF = 1920/2.0 - CENTER_X; //correct the Optical Center as offset from the middle,here img width is 1600 
const float WIDTH_SCALE = 0.5; // scale for x estimation in 3D

using namespace torch::indexing; 
using namespace cv;
using namespace std;
//used for debugging coordinaes with visualization 
void CallBackFunc(int event, int x, int y, int flags, void* userdata)
{
     if  ( event == EVENT_LBUTTONDOWN )
     {
          cout << "Left button of the mouse is clicked - position (" << x << ", " << y << ")" << endl;
     }
     else if  ( event == EVENT_RBUTTONDOWN )
     {
          cout << "Right button of the mouse is clicked - position (" << x << ", " << y << ")" << endl;
     }
     else if  ( event == EVENT_MBUTTONDOWN )
     {
          cout << "Middle button of the mouse is clicked - position (" << x << ", " << y << ")" << endl;
     }
     else if ( event == EVENT_MOUSEMOVE )
     {
          cout << "Mouse move over the window - position (" << x << ", " << y << ")" << endl;

     }
}


class Pipeline { 

torch::Tensor values_x; 
torch::Tensor values_y; 
torch::Tensor tharray; 
//masks for the car and the image edge 
std::vector<std::vector<cv::Point>> car_coordinates; 
std::vector<std::vector<cv::Point>> edge_coordinates; 
//tensor options 
torch::TensorOptions options; 
torch::TensorOptions options_cpu; 
torch::Tensor tharray_gpu; //tharray stores REKTNET output 
torch::Tensor slopes; //slopes for mid point for height computation 
torch::Tensor yolo_out; //yolo outputs include box position and height/width for 3D pose estimation  
torch::Tensor yolo_out_gpu; //GPU yolo outputs include box position and height/width for 3D pose estimation  
torch::Tensor cone_heights; 
PipelineParams params;

public : 

Pipeline(){

//tensors and tensor options 	
options = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA,0);
options_cpu = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU);
values_x = torch::linspace(0,(REKT_SIZE -1.0)/REKT_SIZE, REKT_SIZE, options); 
values_y = torch::linspace(0,(REKT_SIZE -1.0)/REKT_SIZE, REKT_SIZE, options); 
tharray = torch::zeros({BATCH_SIZE_REKT,7,80,80},torch::kFloat32).to(torch::kCPU); //or use kF64
tharray_gpu = torch::zeros({BATCH_SIZE_REKT,7,80,80},torch::kFloat32).to(torch::kCUDA); 
yolo_out = torch::zeros({BATCH_SIZE_REKT,5},torch::kFloat32).to(torch::kCPU);  
yolo_out_gpu = torch::zeros({BATCH_SIZE_REKT,5},torch::kFloat32).to(torch::kCUDA);  
cone_heights = torch::ones({BATCH_SIZE_REKT}, torch::kFloat32).to(torch::kCUDA) * CONE_HEIGHT_SMALL ; //set cone height vector  


//masks

//car mask 
car_coordinates.push_back(std::vector<cv::Point>());
car_coordinates[0].push_back(cv::Point(0,0)); 
//car mask 
/*
car_coordinates[0].push_back(cv::Point(155,1200)); 
car_coordinates[0].push_back(cv::Point(202,1146)); 
car_coordinates[0].push_back(cv::Point(208,1156)); 
car_coordinates[0].push_back(cv::Point(282,1115)); 
car_coordinates[0].push_back(cv::Point(461,1114)); 
car_coordinates[0].push_back(cv::Point(584,1040)); 
car_coordinates[0].push_back(cv::Point(727,1032)); 
car_coordinates[0].push_back(cv::Point(762,980)); 
car_coordinates[0].push_back(cv::Point(811,952)); 
car_coordinates[0].push_back(cv::Point(894,926)); 
car_coordinates[0].push_back(cv::Point(978,922)); 
car_coordinates[0].push_back(cv::Point(1068,931)); 
car_coordinates[0].push_back(cv::Point(1139,964)); 
car_coordinates[0].push_back(cv::Point(1173,988)); 
car_coordinates[0].push_back(cv::Point(1209,1034)); 
car_coordinates[0].push_back(cv::Point(1330,1037)); 
car_coordinates[0].push_back(cv::Point(1437,1114)); 
car_coordinates[0].push_back(cv::Point(1628,1108)); 
car_coordinates[0].push_back(cv::Point(1655,1114)); 
car_coordinates[0].push_back(cv::Point(1714,1157)); 
car_coordinates[0].push_back(cv::Point(1774,1200)); 
*/
//edge mask
edge_coordinates.push_back(std::vector<cv::Point>()); 
edge_coordinates[0].push_back(cv::Point(0,300)); 
edge_coordinates[0].push_back(cv::Point(0,1200)); 
edge_coordinates[0].push_back(cv::Point(1920,1200)); 
edge_coordinates[0].push_back(cv::Point(1920,300)); 
edge_coordinates[0].push_back(cv::Point(1900,300)); 
edge_coordinates[0].push_back(cv::Point(1900,1180)); 
edge_coordinates[0].push_back(cv::Point(20,1180)); 
edge_coordinates[0].push_back(cv::Point(20,300)); 

slopes = torch::ones({BATCH_SIZE_REKT}).to(torch::kCUDA);  
} 

~Pipeline(){}

torch::Tensor flat_softmax(OUT input) {

auto start = std::chrono::system_clock::now();
//convert array to torch tensor and move it to the GPU 
std::memcpy(tharray.data_ptr(),input,sizeof(float)*tharray.numel());
tharray_gpu = tharray.to(torch::kCUDA); 
torch::Tensor flat = tharray_gpu.view({-1,REKT_SIZE*REKT_SIZE}); 
flat = torch::nn::functional::softmax(flat,1);
torch::Tensor hm = flat.view({-1,7,REKT_SIZE,REKT_SIZE}); 
torch::Tensor exp_y = (hm.sum(3) * values_y).sum(-1); 
torch::Tensor exp_x = (hm.sum(2) * values_x).sum(-1); 


auto end = std::chrono::system_clock::now();

return torch::stack({exp_x, exp_y},-1); 
} 




torch::Tensor distance_calculate(const torch::Tensor &data_batch, float (*input)[5], std::vector<int> &box_large_id )
{    
      //read params from file 
      if (!params.fromFile(paramsFilePath))
      {
          std::cerr << "Error reading params file" << std::endl;
      }

     //convert vector of yolo large box indeces  to Tensor 
     torch::Tensor box_idcs = torch::from_blob(box_large_id.data(), {box_large_id.size()}, options_cpu).to(torch::kInt64); 
     torch::Tensor box_idcs_gpu = box_idcs.to(torch::kCUDA); 
     cone_heights = cone_heights.index_put_({box_idcs_gpu},params.cone_large_height);     
     //heights of bounding boxes to Tensor  
     
     std::memcpy(yolo_out.data_ptr(),input,sizeof(float)*yolo_out.numel());
     yolo_out_gpu = yolo_out.to(torch::kCUDA); 
     
     //compute slope for midpoint 
     torch::Tensor slope = -1 * (data_batch.index({"...",6,1}) - data_batch.index({"...",5,1}))/(data_batch.index({"...",6,0}) - data_batch.index({"...",5,0})); 
     //max(0,slope) 
     torch::Tensor relu_data = torch::relu(slope);
     torch::Tensor t = at::nonzero(relu_data);
     slopes = slopes.index_put_({t},-1);

     //midpoint components
     torch::Tensor mid1 = data_batch.index({"...",6,Slice(0,2)}) - data_batch.index({"...",5,Slice(0,2)}); 
     torch::Tensor mid2 = torch::ones({mid1.sizes()[0],2}).to(torch::kCUDA) * 0.5 * torch::stack({slopes,slopes},1);//change slope right here
     torch::Tensor mid3 = data_batch.index({"...",5,Slice(0,2)});
     
     //compute midpoint 
     torch::Tensor mid_pts = mid1 * mid2 + mid3;  
     //----------------------------------TO-DO----------------------------CHANGE HEIGHT of 123---------

     torch::Tensor top_pt = (mid_pts - data_batch.index({"...",0,Slice(0,2)}));  
     torch::Tensor img_h = torch::norm(top_pt,2,1) *  yolo_out_gpu.index({"...",3}); 
     torch::Tensor hbb = img_h * params.pixel_size; 

     torch::Tensor distances = params.focal_length / (hbb * cone_heights);  	 
     
     //compute the x pose considering distance, box width and 
     //torch::Tensor bbox_center  = PIXEL_SIZE * (yolo_out_gpu.index({"...",0}) + (yolo_out_gpu.index({"...",2}) * 0.5) - IMG_HALF);
     //torch::Tensor widths = (bbox_center * distances) /FOCAL_LENGTH;  
      
     //this is the x value of the bounding box middle on the image sensor   
     torch::Tensor camera_x = (yolo_out_gpu.index({"...",0}) + (yolo_out_gpu.index({"...",2}) * 0.5) - params.center_x) * params.pixel_size;  
     //camera_d is the straight line drawn from the distance to the image sensor
     torch::Tensor camera_d = torch::sqrt( camera_x * camera_x + params.focal_length + params.focal_length);

     //calc x coordinates 
     torch::Tensor x = camera_x * distances /(camera_d); 
     //calc y coordinates 
     torch::Tensor y = distances * params.focal_length /(camera_d); 

     std::cout << "distances" << distances << std::endl;
     std::cout << "x" << x << std::endl;
     std::cout << "y" << y << std::endl;
     auto end = std::chrono::system_clock::now();
     return torch::stack({y,x},1);

} 

//void plot_pts(std::vector<Yolo::Detection>& res, std::vector<std::vector<float>> &bbox_vals, torch::Tensor &rektnet, torch::Tensor &distances,cv::Mat &img2,const int &idx)
void plot_pts(std::vector<Yolo::Detection>& res, std::vector<std::vector<float>> &bbox_vals,cv::Mat &img2)
{


//for(size_t i=0; i < idx;i++)
for(size_t i=0; i < res.size();i++)
{ 
        
          //std::vector<float> d(dist[i].data_ptr<float>(), dist[i].data_ptr<float>() + dist[i].numel());	
          //std::vector<float> w(width[i].data_ptr<float>(), width[i].data_ptr<float>() + width[i].numel());	
	  cv::Rect r = get_rect(img2, res[i].bbox);
	  
	   if((int)res[i].class_id == 0){
	   cv::rectangle(img2, r, cv::Scalar(255, 0, 0), 2);
	   }  else if((int)res[i].class_id == 1){
	   cv::rectangle(img2, r, cv::Scalar(0, 165,255 ), 2);
	   } 
	   else if((int)res[i].class_id == 2){
	   cv::rectangle(img2, r, cv::Scalar(0, 0, 255), 2);
	   }else if((int)res[i].class_id == 3){
	   cv::rectangle(img2, r, cv::Scalar(0, 255, 255), 2);
	   }
	   else {
	   cv::rectangle(img2, r, cv::Scalar(0, 255, 0), 2);
	   } 
          
          //cv::putText(img2, std::to_string((int)res[i].class_id), cv::Point(r.x, r.y - 1), cv::FONT_HERSHEY_PLAIN, 1.2, cv::Scalar(0xFF, 0xFF, 0xFF), 2);
	  //std::ostringstream ss; 
	  //std::ostringstream ss1,ss2;
	  //ss1 << floorf(d[0] * 100)/100;
	  //ss2 << floorf(w[0]*100)/100;
	  //ss << ss1.str() << "," << ss2.str(); 
	  //cv::putText(img2, ss.str(), cv::Point(r.x, r.y - 1), cv::FONT_HERSHEY_PLAIN, 1.2, cv::Scalar(0xFF, 0xFF, 0xFF), 2);
          /* 
	  rektnet = rektnet.to(torch::kCPU); 
	  for(int j = 0; j < 7 ;++j)
	  {    
		  std::vector<float> v(rektnet[i][j].data_ptr<float>(), rektnet[i][j].data_ptr<float>() + rektnet[i][j].numel());
		  cv::circle(img2,cv::Point(r.x+int(v[0] * r.width), r.y + int(v[1] * r.height)),2,cv::Scalar(255,0,0),3);  

	  }
	  */

}	
//cv::drawContours(img2,car_coordinates,0, cv::Scalar(255,0,0),cv::FILLED, 8 );
//cv::drawContours(img2,edge_coordinates,0, cv::Scalar(255,0,0),cv::FILLED, 8 );

//Create a window
cv::namedWindow("My Window", 1);

//set the callback function for any mouse event
//cv::setMouseCallback("My Window", CallBackFunc, NULL);
cv::imshow("My Window",img2); 
cv::waitKey(1); 


}


}; 



class CameraNode { 
	private : 
		ros::NodeHandle nh_; 
		image_transport::ImageTransport it_; 
		image_transport::Subscriber image_sub_; 
                YOLO_INF yolov5 = YOLO_INF();
		Rektnet rektnet = Rektnet(10); 
		Pipeline pipeline = Pipeline();

	public : 

		CameraNode(const std::string &topic_name) : it_(nh_) {
		image_sub_ = it_.subscribe(topic_name,1,&CameraNode::chatterCallback,this); 	
		}
		~CameraNode() {}

void chatterCallback(const sensor_msgs::ImageConstPtr &img)  {
	
	cv_bridge::CvImagePtr cv_ptr;
	 try
         {
           cv_ptr = cv_bridge::toCvCopy(img, sensor_msgs::image_encodings::BGR8);
          }
         catch (cv_bridge::Exception& e)
         {
           ROS_ERROR("cv_bridge exception: %s", e.what());
          return;
     	 }
	cv::Mat image; 
	image = cv_ptr->image; 
	std::vector<cv::Mat> imgs = yolov5.inference(image);
	//rektnet inference  
        if (yolov5.no_out != true) {
        //auto out_rekt = rektnet.inference(imgs);  
        //softmax rektnet output and get coordinates from heatmap
        //auto out = pipeline.flat_softmax(out_rekt);
        //get 3D coordinates of x and y with depth estimation  
        //auto dist = pipeline.distance_calculate(out,yolov5.boxes,yolov5.box_large); 
        //pipeline.plot_pts(yolov5.res_sorted,yolov5.box_coords,out,dist,image,yolov5.c);  
        pipeline.plot_pts(yolov5.res_sorted,yolov5.box_coords,image);  
        } 

	else { 
	 cv::imshow("My Window",image); 
	 cv::waitKey(1); 
	} 
        
}



}; 


int main(int argc, char **argv)
{
 ros::init(argc,argv,"camera_sub"); 
 ros::NodeHandle n;  
 //CameraNode cn("/fsds/camera/cam1");  
 CameraNode cn("/pylon_camera_node/image_raw");  
 ros::spin();
 return 0; 
}
