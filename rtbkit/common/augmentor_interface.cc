#include "rtbkit/common/messages.h"
#include "augmentor_interface.h"

using namespace Datacratic;
using namespace RTBKIT;

AugmentorInterface::AugmentorInterface(ServiceBase & parent,
                                 std::string const & serviceName) :
    ServiceBase(serviceName, parent),
    router(nullptr){
}

AugmentorInterface::AugmentorInterface(std::shared_ptr<ServiceProxies> proxies,
                                 std::string const & serviceName) :
    ServiceBase(serviceName, proxies),
    router(nullptr){
}

void AugmentorInterface::setInterfaceName(const std::string &name){
    this->name = name;
}

std::string AugmentorInterface::interfaceName() const {
    return name;
}

void AugmentorInterface::start(){
}

void AugmentorInterface::shutdown(){
}

std::shared_ptr<AugmentorInterface> AugmentorInterface::create(
        std::string serviceName,
        std::shared_ptr<ServiceProxies> const & proxies,
        Json::Value const & json) {

    auto type = json.get("type", "unknown").asString();
    auto factory = PluginInterface<AugmentorInterface>::getPlugin(type);
    
    if(serviceName.empty()) {
        serviceName = json.get("serviceName", "augmentor").asString();
    }

    return std::shared_ptr<AugmentorInterface>(factory(serviceName, proxies, json));
}

