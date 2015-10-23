#include "http_augmentor_interface.h"
#include "jml/db/persistent.h"
#include "soa/service/http_client.h"
#include "soa/utils/generic_utils.h"
#include "rtbkit/common/messages.h"
#include "rtbkit/plugins/bid_request/openrtb_bid_request_parser.h"
#include "rtbkit/openrtb/openrtb_parsing.h"
#include "rtbkit/core/router/router.h"
#include "jml/arch/futex.h"

using namespace Datacratic;
using namespace RTBKIT;

namespace {
    DefaultDescription<OpenRTB::BidRequest> desc;

    std::string httpErrorString(HttpClientError code)  {
        switch (code) {
            #define CASE(code) \
                case code: \
                    return #code;
            CASE(HttpClientError::None)
            CASE(HttpClientError::Unknown)
            CASE(HttpClientError::Timeout)
            CASE(HttpClientError::HostNotFound)
            CASE(HttpClientError::CouldNotConnect)
            CASE(HttpClientError::SendError)
            CASE(HttpClientError::RecvError)
            #undef CASE
        }
        ExcCheck(false, "Invalid code path");
        return "";
    }
}

namespace RTBKIT {

Logging::Category HttpAugmentorInterface::print("HttpAugmentorInterface");
Logging::Category HttpAugmentorInterface::error(
    "HttpAugmentorInterface Error", HttpAugmentorInterface::print);
Logging::Category HttpAugmentorInterface::trace(
    "HttpAugmentorInterface Trace", HttpAugmentorInterface::print);

}

auto HttpAugmentorInterface::readFormat(const std::string& fmt) -> Format {
    if (fmt == "standard") return FMT_STANDARD;
    if (fmt == "datacratic") return FMT_DATACRATIC;
    ExcCheck(false, "unknown format string: " + fmt);
}

HttpAugmentorInterface::HttpAugmentorInterface(std::string serviceName,
                                         std::shared_ptr<ServiceProxies> proxies,
                                         Json::Value const & json)
        : AugmentorInterface(proxies, serviceName),
        allAugmentors(0),
        idle_(1),
        inbox(65536) {

    int augmentorHttpActiveConnections = 0;
    
    std::string augHost;
    std::string augPath;
    std::string augName;
    std::string augFormat;

    try {
        auto augs = json["augmentors"];
        for (const auto &conf: augs) {
            augHost = conf["host"].asString();
            augPath = conf["path"].asString();
            augName = conf["name"].asString();
            augmentorHttpActiveConnections = conf.get("httpActiveConnections", 1024).asInt();
            augFormat = readFormat(conf.get("format", "standard").asString());

            std::string url = augHost + augPath;
            AugmentorInstanceInfo inst(url);
            AugmentorInfoMap::iterator it;
            if((it = augmentors.find(augName)) == augmentors.end()){
                std::shared_ptr<AugmentorInfo> info = std::make_shared<AugmentorInfo>(augName);
                info->instances.push_back(inst);
                augmentors.insert(make_pair(augName, info));
            }else{
                it->second->instances.push_back(inst);
            }
        }
    } catch (const std::exception & e) {
        THROW(error) << "configuration file is invalid" << std::endl
            << "usage : " << std::endl
            << "{" << std::endl
            << "\t\"type\": \"http\"," << std::endl
            << "\t\"augmentors\": [" << std::endl
            << "\t\t{" << std::endl
            << "\t\t\t\"name\": \"augmentor01\"," << std::endl
            << "\t\t\t\"host\": \"http://localhost\"," << std::endl
            << "\t\t\t\"path\": \"/augmentor01\"" << std::endl
            << "\t\t}," << std::endl
            << "\t\t{" << std::endl
            << "\t\t\t\"name\": \"augmentor02\"," << std::endl
            << "\t\t\t\"host\": \"http://localhost\"," << std::endl
            << "\t\t\t\"path\": \"/augmentor02\"" << std::endl
            << "\t\t}"<< std::endl
            << "\t]" << std::endl
            << "}" << std::endl;
    }    

    httpClientAugmentor.reset(new HttpClient(augHost, augmentorHttpActiveConnections));
    
    /* We do not want curl to add an extra "Expect: 100-continue" HTTP header
     * and then pay the cost of an extra HTTP roundtrip. Thus we remove this
     * header
     */
    httpClientAugmentor->sendExpect100Continue(false);
    loop.addSource("HttpAugmentorInterface::httpClientAugmentor", httpClientAugmentor);

    loop.addPeriodic("HttpAugmentorInterface::reportQueues", 1.0, [=](uint64_t) {
        recordLevel(httpClientAugmentor->queuedRequests(), "queuedRequests");
    });
    
}

HttpAugmentorInterface::~HttpAugmentorInterface()
{
    shutdown();
}

void HttpAugmentorInterface::init(Router * r){
    router = r;
 
    inbox.onEvent = [&] (const std::shared_ptr<Entry>& entry)
        {
            doAugmentation(entry);
        };

    loop.addSource("AugmentationLoop::inbox", inbox);
    
    loop.addPeriodic("AugmentationLoop::checkExpiries", 0.001,
                [=] (int) { checkExpiries(); });

    loop.addPeriodic("AugmentationLoop::recordStats", 0.977,
                [=] (int) { recordStats(); });
}

void HttpAugmentorInterface::start() {
    loop.start();
}

void HttpAugmentorInterface::sleepUntilIdle()
{
    while (!idle_)
        ML::futex_wait(idle_, 0);
}

void HttpAugmentorInterface::shutdown() {
    loop.shutdown();
}

size_t HttpAugmentorInterface::numAugmenting() const {
    return augmenting.size();
}

void HttpAugmentorInterface::augment(
        const std::shared_ptr<AugmentorInterface::AugmentationInfo> & info,
        Date timeout, const OnFinished & onFinished){
    Date now = Date::now();

    auto entry = std::make_shared<Entry>();
    entry->onFinished = onFinished;
    entry->info = info;
    entry->timeout = timeout;

    // Get a set of all augmentors
    std::set<std::string> augmentors;

    // Now go through and find all of the bidders
    for (unsigned i = 0;  i < info->potentialGroups.size();  ++i) {
        const GroupPotentialBidders & group = info->potentialGroups[i];
        for (unsigned j = 0;  j < group.size();  ++j) {
            const PotentialBidder & bidder = group[j];
            const AgentConfig & config = *bidder.config;
            for (unsigned k = 0;  k < config.augmentations.size();  ++k) {
                const std::string & name = config.augmentations[k].name;
                augmentors.insert(name);
                entry->augmentorAgents[name].insert(bidder.agent);
            }
        }
    }

    // Find which ones are actually available...
    GcLock::SharedGuard guard(allAugmentorsGc);
    const AllAugmentorInfo * ai = allAugmentors;
    
    ExcAssert(ai);

    auto it1 = augmentors.begin(), end1 = augmentors.end();
    auto it2 = ai->begin(), end2 = ai->end();

    while (it1 != end1 && it2 != end2) {
        if (*it1 == it2->name) {
            // Augmentor we need to run
            //cerr << "augmenting with " << it2->name << endl;
            recordEvent("augmentation.request");
            std::string eventName = "augmentor." + it2->name + ".request";
            recordEvent(eventName.c_str());
            
            entry->outstanding.insert(*it1);

            ++it1;
            ++it2;
        }else if (*it1 < it2->name){
            // Augmentor is not available
            //cerr << "augmentor " << *it1 << " is not available" << endl;
            ++it1;
        }else if (it2->name < *it1){
            // Augmentor is not required
            //cerr << "augmentor " << it2->name << " is not required" << endl;
            ++it2;
        } else throw ML::Exception("logic error traversing augmentors");
    }

    if(entry->outstanding.empty()){
        // No augmentors required... run the auction straight away
        onFinished(info);
    }else{
        //cerr << "putting in inbox" << endl;
        inbox.push(entry);
    }
}

void HttpAugmentorInterface::registerLoopMonitor(LoopMonitor *monitor) const {
    monitor->addMessageLoop("httpAugmentorInterfaceLoop", &loop);
}

void HttpAugmentorInterface::doAugmentation(const std::shared_ptr<Entry> & entry){
    /*TODO*/
}

void HttpAugmentorInterface::recordStats(){
    for (auto it = augmentors.begin(), end = augmentors.end();
         it != end;  ++it)
    {
        size_t inFlights = 0;
        for (const auto& instance : it->second->instances)
            inFlights += instance.numInFlight;

        recordLevel(inFlights, "augmentor.%s.numInFlight", it->first);
    }
}

void HttpAugmentorInterface::checkExpiries(){
    Date now = Date::now();

    auto onExpired = [&] (const Id & id,
                          const std::shared_ptr<Entry> & entry) -> Date
        {
            for (auto it = entry->outstanding.begin(),
                     end = entry->outstanding.end();
                 it != end; ++it)
            {
                recordHit("augmentor.%s.expiredTooLate", *it);
            }
                
            augmentationExpired(id, *entry);
            return Date();
        };

    if (augmenting.earliest <= now)
        augmenting.expire(onExpired, now);

    if (augmenting.empty() && !idle_) {
        idle_ = 1;
        ML::futex_wake(idle_);
    }
}

void
HttpAugmentorInterface::augmentationExpired(const Id & id, const Entry & entry)
{
    entry.onFinished(entry.info);
}   
