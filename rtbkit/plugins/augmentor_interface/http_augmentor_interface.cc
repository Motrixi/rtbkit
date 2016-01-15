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
        allAugmentors(new AllAugmentorInfo),
        idle_(1),
        inbox(65536) {

    std::cerr << "HttpAugmentorInterface constructor" << std::endl;
    int augmentorHttpActiveConnections = 0;
    
    std::string augHost;
    std::string augPath;
    std::string augName;
    std::string augFormat;
    int maxInFlight;

    try {
        auto augs = json["augmentors"];
        for (const auto &conf: augs) {
            augHost = conf["host"].asString();
            augPath = conf["path"].asString();
            augName = conf["name"].asString();
            maxInFlight = conf["maxInFlight"].asInt();
            augmentorHttpActiveConnections = conf.get("httpActiveConnections", 1024).asInt();
            augFormat = readFormat(conf.get("format", "standard").asString());

            AugmentorInstanceInfo inst(augHost, augPath, maxInFlight);
            inst.httpClientAugmentor.reset(
                new HttpClient(augHost, augmentorHttpActiveConnections));

            /* We do not want curl to add an extra "Expect: 100-continue" HTTP header
             * and then pay the cost of an extra HTTP roundtrip. Thus we remove this
             * header
             */
            inst.httpClientAugmentor->sendExpect100Continue(false);
            loop.addSource("HttpAugmentorInterface::httpClientAugmentor",
                                inst.httpClientAugmentor);

            loop.addPeriodic("HttpAugmentorInterface::reportQueues", 1.0, [=](uint64_t) {
                recordLevel(inst.httpClientAugmentor->queuedRequests(), "queuedRequests");
            });

            AugmentorInfoEntry e;
            e.name = augName;

            AugmentorInfoMap::iterator it;
            if((it = augmentors.find(augName)) == augmentors.end()){
                std::shared_ptr<AugmentorInfo> info = std::make_shared<AugmentorInfo>(augName);
                info->instances.push_back(inst);
                augmentors.insert(make_pair(augName, info));
                std::cerr << "inserting : " << augName << std::endl;
                e.info = info;
            }else{
                it->second->instances.push_back(inst);
                e.info = it->second;
            }
            allAugmentors->push_back(e);
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
            << "\t\t\t\"maxInFlight\": 1024" << std::endl
            << "\t\t}," << std::endl
            << "\t\t{" << std::endl
            << "\t\t\t\"name\": \"augmentor02\"," << std::endl
            << "\t\t\t\"host\": \"http://localhost\"," << std::endl
            << "\t\t\t\"path\": \"/augmentor02\"" << std::endl
            << "\t\t\t\"maxInFlight\": 1024" << std::endl
            << "\t\t}"<< std::endl
            << "\t]" << std::endl
            << "}" << std::endl;
    }
    
}

HttpAugmentorInterface::~HttpAugmentorInterface()
{
    shutdown();
}

void HttpAugmentorInterface::init(){
 
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
    
    //XXX : fix nemi
    ExcAssert(ai);

    for(auto it = augmentors.begin(); it != augmentors.end(); ++it){
            // Augmentor we need to run
            //cerr << "augmenting with " << it2->name << endl;
            recordEvent("augmentation.request");
            std::string eventName = "augmentor." + *it + ".request";
            recordEvent(eventName.c_str());
            
            entry->outstanding.insert(*it);
    }

    if(entry->outstanding.empty()){
        // No augmentors required... run the auction straight away
        onFinished(info);
    }else{
        inbox.push(entry);
    }
}

void HttpAugmentorInterface::registerLoopMonitor(LoopMonitor *monitor) const {
    monitor->addMessageLoop("httpAugmentorInterfaceLoop", &loop);
}

void HttpAugmentorInterface::doAugmentation(const std::shared_ptr<Entry> & entry){
    Date now = Date::now();

    if (augmenting.count(entry->info->auction->id)) {
        std::stringstream ss;
        ss << "AugmentationLoop: duplicate auction id detected "
            << entry->info->auction->id << std::endl;
        std::cerr << ss.str();
        recordHit("duplicateAuction");
        std::cerr << "duplicate auction" << std::endl;
        return;
    }

    bool sentToAugmentor = false;

    for (auto it = entry->outstanding.begin(), end = entry->outstanding.end();
         it != end;  ++it)
    {
        std::set<std::string> agents = entry->augmentorAgents[*it];
        bool success = sendAugmentMessage(*it, entry, agents);
        if(success)
            sentToAugmentor = true;
    }

    if (sentToAugmentor){
        augmenting.insert(entry->info->auction->id, entry, entry->timeout);
    }
    else entry->onFinished(entry->info);

    recordLevel(Date::now().secondsSince(now), "requestTimeMs");

    idle_ = 0;
}

void
HttpAugmentorInterface::
processOKResponse(const std::string & headers,
                std::string &body,
                const std::string & aid,
                std::string & augmentor,
                const std::string & name,
                std::string & augmentation,
                AugmentationList & augmentationList)
{
    RestParams headers_;
    headers_ = RestParams::fromString(headers);

    // get the version
    std::string version = headers_.getValue("X-Rtbkit-Protocol-Version");
    ExcCheckEqual(version, "1.0", "unknown response version");

    // get the timestamp
    std::string t = headers_.getValue("X-Rtbkit-Timestamp");
    Date startTime = Date::parseSecondsSinceEpoch(t);
    // get the auction id
    std::string auctionid = headers_.getValue("X-Rtbkit-Auction-Id");
    ExcCheckEqual(aid, auctionid, "auction id is not the same");
    Id id(auctionid);

    // get all the augmentation data
    augmentor = headers_.getValue("X-Rtbkit-Augmentor-Name");
    ExcCheckEqual(name, augmentor, "augmentor name is not the same");
    augmentation = body;
    ML::Timer timer;
    if (augmentation != "" && augmentation != "null") {
        try {
            Json::Value augmentationJson;

            JML_TRACE_EXCEPTIONS(false);
            augmentationJson = Json::parse(augmentation);
            augmentationList = AugmentationList::fromJson(augmentationJson);
        } catch (const std::exception & exc) {
            std::string eventName = "augmentor." + augmentor
                + ".responseParsingExceptions";
            recordEvent(eventName.c_str(), ET_COUNT);
        }
    }

    recordLevel(timer.elapsed_wall(), "responseParseTimeMs");

    {
        double timeTakenMs = startTime.secondsUntil(Date::now()) * 1000.0;
        std::string eventName = "augmentor." + augmentor + ".timeTakenMs";
        recordEvent(eventName.c_str(), ET_OUTCOME, timeTakenMs);
    }

    {
        double responseLength = augmentation.size();
        std::string eventName = "augmentor." + augmentor + ".responseLengthBytes";
        recordEvent(eventName.c_str(), ET_OUTCOME, responseLength);
    }
}

HttpAugmentorInterface::AugmentorInstanceInfo*
HttpAugmentorInterface::
pickInstance(AugmentorInfo& aug)
{
    AugmentorInstanceInfo* instance = nullptr;
    int minInFlights = std::numeric_limits<int>::max();

    std::stringstream ss;
    for (auto it = aug.instances.begin(), end = aug.instances.end();
         it != end; ++it)
    {
        if (it->numInFlight >= minInFlights) continue;
        if (it->numInFlight >= it->maxInFlight) continue;

        instance = &(*it);
        minInFlights = it->numInFlight;
    }

    if (instance) instance->numInFlight++;
    return instance;
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

//
// factory
//

bool
HttpAugmentorInterface::sendAugmentMessage(
                const std::string& augmentor,
                const std::shared_ptr<AugmentorInterface::Entry> & entry,
                std::set<std::string> agents){

    auto & aug = *augmentors[augmentor];
    const AugmentorInstanceInfo* instance = pickInstance(aug);
    if (!instance) {
        recordHit("augmentor.%s.skippedTooManyInFlight", augmentor);
        std::cerr << "skippedTooManyInFlight" << std::endl;
        return false;
    }
    recordHit("augmentor.%s.instances.%s.request", augmentor, instance->path);

    for(auto it = agents.begin(); it != agents.end(); ++it)
        entry->info->auction->request->ext["agents"].append(*it);


    std::string name = augmentor;
    std::string aid = entry->info->auction->id.toString();
    Date date = Date::now();
    std::string path = instance->path;

    HttpRequest::Content reqContent {
            entry->info->auction->request->toJsonStr(), "application/json" };

    RestParams headers { { "X-Openrtb-Version", "2.1" } };

    auto callbacks = std::make_shared<HttpClientSimpleCallbacks>(
        [=, &entry](const HttpRequest &request, HttpClientError errorCode,
            int statusCode, const std::string && headers, std::string &&body)
        {
            recordEvent("augmentation.response");
            std::string augmentor;
            std::string augmentation;
            AugmentationList augmentationList;

            if(errorCode == HttpClientError::None && statusCode == 200){
                try{
                    processOKResponse(headers, body, aid, augmentor, name,
                                    augmentation, augmentationList);
                }catch(...){
                    augmentor = name;
                    recordHit("augmentor.%s.exceptionOnResponse", augmentor);
                }
            }else{
                augmentor = name;
            }
            Id id(aid);
            auto augmentorIt = augmentors.find(augmentor);
            if (augmentorIt != augmentors.end()) {
                auto instance = augmentorIt->second->findInstance(path);
                if (instance) instance->numInFlight--;
            }
            auto augmentingIt = augmenting.find(id);
            if (augmentingIt == augmenting.end()) {
                recordHit("augmentation.unknown");
                recordHit("augmentor.%s.unknown", augmentor, path);
                recordHit("augmentor.%s.instances.%s.unknown", augmentor, path);
                return;
            }

            auto& entry = *augmentingIt;

            const char* eventType =
                (augmentation == "" || augmentation == "null") ?
                "nullResponse" : "validResponse";
            recordHit("augmentor.%s.%s", augmentor, eventType);
            recordHit("augmentor.%s.instances.%s.%s", augmentor, path, eventType);

            auto& auctionAugs = entry.second->info->auction->augmentations;
            auctionAugs[augmentor].mergeWith(augmentationList);

            entry.second->outstanding.erase(augmentor);
            if (entry.second->outstanding.empty()) {
                entry.second->onFinished(entry.second->info);
                augmenting.erase(augmentingIt);
            }
        }
    );

    instance->httpClientAugmentor->post(path, callbacks, reqContent, { }, headers);
    return true;
}

namespace {

struct AtInit {
    AtInit()
    {
      PluginInterface<AugmentorInterface>::registerPlugin("http",
          [](std::string const &serviceName,
             std::shared_ptr<ServiceProxies> const &proxies,
             Json::Value const &json)
          {
              return new HttpAugmentorInterface(serviceName, proxies, json);
          });
    }
} atInit;

}

