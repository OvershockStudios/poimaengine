// SPDX-License-Identifier: Apache-2.0
#include "poima/collision_queries.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
namespace poima {
namespace {
void require(bool value,const char* message){if(!value)throw std::invalid_argument(message);}
void bounded(double value,double low,double high,const char* message){require(std::isfinite(value) && value>=low && value<=high,message);}
void rotation(const std::array<double,4>& value){double norm=0;for(double x:value){bounded(x,-1.00001,1.00001,"Capsule rotation must be finite and normalized.");norm+=x*x;}require(std::abs(norm-1)<=1e-5,"Capsule rotation must be normalized.");}
void dimensions(double radius,double half_height){bounded(radius,.001,10,"Capsule radius must be .001..10 metres.");bounded(half_height,0,10,"Capsule cylinder half-height must be 0..10 metres.");}
}
void validate_capsule_cast_request(const CapsuleCastRequest& request){
    for(double x:request.position)bounded(x,-1e6,1e6,"Capsule position must be within +/-1000km.");
    rotation(request.rotation);dimensions(request.radius,request.half_height);
    bounded(request.distance,0,10000,"Capsule distance must be 0..10000 metres.");
    double length=0;for(double x:request.direction){bounded(x,-1e6,1e6,"Capsule direction must be finite and bounded.");length+=x*x;}
    require(length>=1e-24,"Capsule direction cannot be zero or near zero, including overlap queries.");
    if(request.distance>0){length=std::sqrt(length);for(std::size_t k=0;k<3;++k)bounded(request.position[k]+request.direction[k]/length*request.distance,-1e6,1e6,"Capsule endpoint must be within +/-1000km.");}
    require(request.ignore.size()<=128,"At most128 ignored capsule entities.");std::set<std::string> unique;
    for(const auto& id:request.ignore)require(!id.empty() && id.size()<=256 && id.find('\0')==std::string::npos && unique.insert(id).second,"Capsule ignore IDs must be unique nonempty bounded identities.");
}
void validate_viewmodel_clearance_profile(const ViewmodelClearanceProfile& profile){
    for(double x:profile.position)bounded(x,-10,10,"Clearance local position must be within +/-10 metres.");
    rotation(profile.rotation);dimensions(profile.radius,profile.half_height);
    bounded(profile.max_retraction,.001,10,"Clearance retraction must be .001..10 metres.");
    bounded(profile.skin,.0001,std::min(profile.radius,profile.max_retraction),"Clearance skin must be .0001..min(radius,max_retraction).");
}
}
