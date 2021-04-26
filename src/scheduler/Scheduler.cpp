#include <faabric/proto/faabric.pb.h>
#include <faabric/redis/Redis.h>
#include <faabric/scheduler/FunctionCallClient.h>
#include <faabric/scheduler/Scheduler.h>
#include <faabric/scheduler/SnapshotClient.h>
#include <faabric/snapshot/SnapshotRegistry.h>
#include <faabric/util/environment.h>
#include <faabric/util/func.h>
#include <faabric/util/logging.h>
#include <faabric/util/random.h>
#include <faabric/util/snapshot.h>
#include <faabric/util/testing.h>
#include <faabric/util/timing.h>

#include <unordered_set>

#define FLUSH_TIMEOUT_MS 10000

using namespace faabric::util;

namespace faabric::scheduler {

int decrementAboveZero(int input)
{
    return std::max<int>(input - 1, 0);
}

Scheduler::Scheduler()
  : thisHost(faabric::util::getSystemConfig().endpointHost)
  , conf(faabric::util::getSystemConfig())
{
    bindQueue = std::make_shared<InMemoryMessageQueue>();

    // Set up the initial resources
    int cores = faabric::util::getUsableCores();
    thisHostResources.set_cores(cores);
}

std::unordered_set<std::string> Scheduler::getAvailableHosts()
{
    redis::Redis& redis = redis::Redis::getQueue();
    return redis.smembers(AVAILABLE_HOST_SET);
}

void Scheduler::addHostToGlobalSet(const std::string& host)
{
    redis::Redis& redis = redis::Redis::getQueue();
    redis.sadd(AVAILABLE_HOST_SET, host);
}

void Scheduler::removeHostFromGlobalSet(const std::string& host)
{
    redis::Redis& redis = redis::Redis::getQueue();
    redis.srem(AVAILABLE_HOST_SET, host);
}

void Scheduler::addHostToGlobalSet()
{
    redis::Redis& redis = redis::Redis::getQueue();
    redis.sadd(AVAILABLE_HOST_SET, thisHost);
}

void Scheduler::reset()
{
    // Reset queue map
    for (const auto& iter : queueMap) {
        iter.second->reset();
    }
    queueMap.clear();

    // Ensure host is set correctly
    thisHost = faabric::util::getSystemConfig().endpointHost;

    // Clear queues
    bindQueue->reset();

    // Reset resources
    thisHostResources = faabric::HostResources();
    thisHostResources.set_cores(faabric::util::getUsableCores());

    // Reset scheduler state
    registeredHosts.clear();
    faasletCounts.clear();
    inFlightCounts.clear();

    // Records
    recordedMessagesAll.clear();
    recordedMessagesLocal.clear();
    recordedMessagesShared.clear();
}

void Scheduler::shutdown()
{
    reset();

    removeHostFromGlobalSet(thisHost);
}

long Scheduler::getFunctionInFlightCount(const faabric::Message& msg)
{
    const std::string funcStr = faabric::util::funcToString(msg, false);
    return inFlightCounts[funcStr];
}

long Scheduler::getFunctionFaasletCount(const faabric::Message& msg)
{
    const std::string funcStr = faabric::util::funcToString(msg, false);
    return faasletCounts[funcStr];
}

int Scheduler::getFunctionRegisteredHostCount(const faabric::Message& msg)
{
    const std::string funcStr = faabric::util::funcToString(msg, false);
    return (int)registeredHosts[funcStr].size();
}

std::unordered_set<std::string> Scheduler::getFunctionRegisteredHosts(
  const faabric::Message& msg)
{
    const std::string funcStr = faabric::util::funcToString(msg, false);
    return registeredHosts[funcStr];
}

void Scheduler::removeRegisteredHost(const std::string& host,
                                     const faabric::Message& msg)
{
    const std::string funcStr = faabric::util::funcToString(msg, false);
    registeredHosts[funcStr].erase(host);
}

void Scheduler::forceEnqueueMessage(const faabric::Message& msg)
{
    std::shared_ptr<faabric::BatchExecuteRequest> req =
      faabric::util::batchExecFactory();
    *req->add_messages() = msg;
    std::shared_ptr<InMemoryBatchQueue> queue = getFunctionQueue(msg);
    std::vector<int> idxs = { 0 };
    queue->enqueue(std::make_pair(idxs, req));
}

faabric::Message Scheduler::getNextMessageForFunction(
  const faabric::Message& msg,
  int timeout)
{
    std::shared_ptr<InMemoryBatchQueue> queue = getFunctionQueue(msg);
    MessageTask task = queue->dequeue(timeout);

    if (task.second->type() == faabric::BatchExecuteRequest::THREADS) {
        throw std::runtime_error(
          "Should not be using getNextMessageForFunction to dequeue batch "
          "thread messages");
    }

    if (task.first.size() != 1) {
        throw std::runtime_error(
          "Should not be using getNextMessageForFunction to dequeue batches "
          "with more than one function");
    }

    int msgIdx = task.first.at(0);
    return task.second->messages().at(msgIdx);
}

std::shared_ptr<InMemoryBatchQueue> Scheduler::getFunctionQueue(
  const faabric::Message& msg)
{
    std::string funcStr = faabric::util::funcToString(msg, false);

    // This will be called from within something holding the lock
    if (queueMap.count(funcStr) == 0) {
        if (queueMap.count(funcStr) == 0) {
            auto mq = std::make_shared<InMemoryBatchQueue>();
            queueMap.emplace(std::make_pair(funcStr, mq));
        }
    }

    return queueMap[funcStr];
}

std::shared_ptr<InMemoryBatchQueue> Scheduler::getFunctionQueue(
  const std::shared_ptr<faabric::BatchExecuteRequest> req)
{
    return getFunctionQueue(req->messages(0));
}

void Scheduler::notifyCallFinished(const faabric::Message& msg)
{
    faabric::util::FullLock lock(mx);

    const std::string funcStr = faabric::util::funcToString(msg, false);

    inFlightCounts[funcStr] = decrementAboveZero(inFlightCounts[funcStr]);

    int newInFlight = decrementAboveZero(thisHostResources.functionsinflight());
    thisHostResources.set_functionsinflight(newInFlight);
}

void Scheduler::notifyFaasletFinished(const faabric::Message& msg)
{
    faabric::util::FullLock lock(mx);
    const std::string funcStr = faabric::util::funcToString(msg, false);

    auto logger = faabric::util::getLogger();
    // Unregister this host if not master and no more faaslets assigned to
    // this function
    if (msg.masterhost().empty()) {
        logger->error("Message {} without master host set", funcStr);
        throw std::runtime_error("Message without master host");
    }

    // Update the faaslet count
    int oldCount = faasletCounts[funcStr];
    if (oldCount > 0) {
        int newCount = faasletCounts[funcStr] - 1;
        faasletCounts[funcStr] = newCount;

        // Unregister if this was the last faaslet for that function
        bool isMaster = thisHost == msg.masterhost();
        if (newCount == 0 && !isMaster) {
            faabric::UnregisterRequest req;
            req.set_host(thisHost);
            *req.mutable_function() = msg;

            FunctionCallClient c(msg.masterhost());
            c.unregister(req);
        }
    }

    // Update bound executors on this host
    int newBoundExecutors =
      decrementAboveZero(thisHostResources.boundexecutors());
    thisHostResources.set_boundexecutors(newBoundExecutors);
}

std::shared_ptr<InMemoryMessageQueue> Scheduler::getBindQueue()
{
    return bindQueue;
}

Scheduler& getScheduler()
{
    // Note that this ref is shared between all faaslets on the given host
    static Scheduler scheduler;
    return scheduler;
}

std::vector<std::string> Scheduler::callFunctions(
  std::shared_ptr<faabric::BatchExecuteRequest> req,
  bool forceLocal)
{
    auto logger = faabric::util::getLogger();

    // Extract properties of the request
    int nMessages = req->messages_size();
    bool isThreads = req->type() == req->THREADS;
    std::vector<std::string> executed(nMessages);

    // Note, we assume all the messages are for the same function and master
    const faabric::Message& firstMsg = req->messages().at(0);
    std::string funcStr = faabric::util::funcToString(firstMsg, false);
    std::string masterHost = firstMsg.masterhost();
    if (masterHost.empty()) {
        std::string funcStrWithId = faabric::util::funcToString(firstMsg, true);
        logger->error("Request {} has no master host", funcStrWithId);
        throw std::runtime_error("Message with no master host");
    }

    // TODO - more granular locking, this is incredibly conservative
    faabric::util::FullLock lock(mx);

    // We want to dispatch remote calls here, and record what's left to be done
    // locally
    std::vector<int> localIdxs;
    if (!forceLocal && masterHost != thisHost) {
        // If we're not the master host, we need to forward the request back to
        // the master host. This will only happen if a nested batch execution
        // happens.
        logger->debug(
          "Forwarding {} {} back to master {}", nMessages, funcStr, masterHost);

        FunctionCallClient c(masterHost);
        c.executeFunctions(req);

    } else if (!forceLocal) {
        // At this point we know we're the master host, and we've not been
        // asked to force full local execution.

        // For threads/ processes we need to have a snapshot key and be
        // ready to push the snapshot to other hosts
        faabric::util::SnapshotData snapshotData;
        std::string snapshotKey = firstMsg.snapshotkey();
        bool snapshotNeeded =
          req->type() == req->THREADS || req->type() == req->PROCESSES;

        if (snapshotNeeded && snapshotKey.empty()) {
            logger->error("No snapshot provided for {}", funcStr);
            throw std::runtime_error(
              "Empty snapshot for distributed threads/ processes");
        } else if (snapshotNeeded) {
            snapshotData =
              faabric::snapshot::getSnapshotRegistry().getSnapshot(snapshotKey);
        }

        // Work out how many we can handle locally
        int nLocally;
        {
            int cores = thisHostResources.cores();

            // Work out available cores, flooring at zero
            int available = cores - thisHostResources.functionsinflight();
            available = std::max<int>(available, 0);

            // Claim as many as we can
            nLocally = std::min<int>(available, nMessages);
        }

        // Handle those that can be executed locally
        for (int i = 0; i < nLocally; i++) {
            localIdxs.emplace_back(i);
            executed.at(i) = thisHost;
        }

        // If some are left, we need to distribute
        int offset = nLocally;
        if (offset < nMessages) {
            // At this point we have a remainder, so we need to distribute
            // the rest over the registered hosts for this function
            std::unordered_set<std::string>& thisRegisteredHosts =
              registeredHosts[funcStr];

            // Schedule the remainder on these other hosts
            for (auto& h : thisRegisteredHosts) {
                int nOnThisHost =
                  scheduleFunctionsOnHost(h, req, executed, offset);

                offset += nOnThisHost;
                if (offset >= nMessages) {
                    break;
                }
            }
        }

        if (offset < nMessages) {
            // At this point we know we need to enlist unregistered hosts
            std::unordered_set<std::string> allHosts = getAvailableHosts();
            std::unordered_set<std::string>& thisRegisteredHosts =
              registeredHosts[funcStr];

            for (auto& h : allHosts) {
                // Skip if already registered
                if (thisRegisteredHosts.find(h) != thisRegisteredHosts.end()) {
                    continue;
                }

                // Skip if this host
                if (h == thisHost) {
                    continue;
                }

                // Schedule functions on the host
                int nOnThisHost =
                  scheduleFunctionsOnHost(h, req, executed, offset);

                // Register the host if it's exected a function
                if (nOnThisHost > 0) {
                    logger->debug("Registering {} for {}", h, funcStr);
                    thisRegisteredHosts.insert(h);
                }

                offset += nOnThisHost;
                if (offset >= nMessages) {
                    break;
                }
            }
        }

        // At this point there's no more capacity in the system, so we
        // just need to execute locally
        for (; offset < nMessages; offset++) {
            localIdxs.emplace_back(offset);
            executed.at(offset) = thisHost;
        }
    }

    // Schedule this message locally. For threads we only need one Faaslet, for
    // anything else ideally we want one Faaslet per function but may have to
    // queue
    logger->debug("Scheduling {} messages locally", localIdxs.size());
    auto funcQueue = this->getFunctionQueue(firstMsg);
    incrementInFlightCount(firstMsg, localIdxs.size());
    funcQueue->enqueue(std::make_pair(localIdxs, req));

    // Add faaslets if need be
    if (isThreads) {
        addFaasletsForBatch(firstMsg);
    }
    if (!isThreads) {
        addFaaslets(firstMsg);
    }

    // Accounting
    for (int i = 0; i < nMessages; i++) {
        std::string executedHost = executed.at(i);
        faabric::Message msg = req->messages().at(i);

        // Log results if in test mode
        if (faabric::util::isTestMode()) {
            recordedMessagesAll.push_back(msg.id());
            if (executedHost.empty() || executedHost == thisHost) {
                recordedMessagesLocal.push_back(msg.id());
            } else {
                recordedMessagesShared.emplace_back(executedHost, msg.id());
            }
        }
    }

    return executed;
}

void Scheduler::broadcastSnapshotDelete(const faabric::Message& msg,
                                        const std::string& snapshotKey)
{

    std::string funcStr = faabric::util::funcToString(msg, false);
    std::unordered_set<std::string>& thisRegisteredHosts =
      registeredHosts[funcStr];

    for (auto host : thisRegisteredHosts) {
        SnapshotClient c(host);
        c.deleteSnapshot(snapshotKey);
    }
}

int Scheduler::scheduleFunctionsOnHost(
  const std::string& host,
  std::shared_ptr<faabric::BatchExecuteRequest> req,
  std::vector<std::string>& records,
  int offset)
{
    auto logger = faabric::util::getLogger();
    const faabric::Message& firstMsg = req->messages().at(0);
    std::string funcStr = faabric::util::funcToString(firstMsg, false);

    int nMessages = req->messages_size();
    int remainder = nMessages - offset;

    // Execute as many as possible to this host
    faabric::HostResources r = getHostResources(host);
    int available = r.cores() - r.functionsinflight();

    // Drop out if none available
    if (available <= 0) {
        logger->debug("Not scheduling {} on {}, no resources", funcStr, host);
        return 0;
    }

    // Set up new request
    std::shared_ptr<faabric::BatchExecuteRequest> hostRequest =
      faabric::util::batchExecFactory();
    hostRequest->set_snapshotkey(req->snapshotkey());
    hostRequest->set_snapshotsize(req->snapshotsize());
    hostRequest->set_type(req->type());

    // Add messages
    int nOnThisHost = std::min<int>(available, remainder);
    for (int i = offset; i < (offset + nOnThisHost); i++) {
        *hostRequest->add_messages() = req->messages().at(i);
        records.at(i) = host;
    }

    // Push the snapshot if necessary
    if (req->type() == req->THREADS || req->type() == req->PROCESSES) {
        std::string snapshotKey = firstMsg.snapshotkey();
        SnapshotClient c(host);
        const SnapshotData& d =
          snapshot::getSnapshotRegistry().getSnapshot(snapshotKey);
        c.pushSnapshot(snapshotKey, d);
    }

    logger->debug(
      "Sending {} of {} {} to {}", nOnThisHost, nMessages, funcStr, host);

    FunctionCallClient c(host);
    c.executeFunctions(hostRequest);

    return nOnThisHost;
}

void Scheduler::callFunction(faabric::Message& msg, bool forceLocal)
{
    // TODO - avoid this copy
    auto req = faabric::util::batchExecFactory();
    *req->add_messages() = msg;

    // Specify that this is a normal function, not a thread
    req->set_type(req->FUNCTIONS);

    // Make the call
    callFunctions(req, forceLocal);
}

std::vector<unsigned int> Scheduler::getRecordedMessagesAll()
{
    return recordedMessagesAll;
}

std::vector<unsigned int> Scheduler::getRecordedMessagesLocal()
{
    return recordedMessagesLocal;
}

std::vector<std::pair<std::string, unsigned int>>
Scheduler::getRecordedMessagesShared()
{
    return recordedMessagesShared;
}

void Scheduler::incrementInFlightCount(const faabric::Message& msg, int count)
{
    const std::string funcStr = faabric::util::funcToString(msg, false);

    inFlightCounts[funcStr] += count;

    thisHostResources.set_functionsinflight(
      thisHostResources.functionsinflight() + count);
}

void Scheduler::addFaasletsForBatch(const faabric::Message& msg)
{
    auto logger = faabric::util::getLogger();

    // Check whether we already have a faaslet for this function on this host
    const std::string funcStr = faabric::util::funcToString(msg, false);

    // If we've not got one faaslet per in-flight function, add one
    int nFaaslets = faasletCounts[funcStr];
    if (nFaaslets == 0) {
        doAddFaaslets(msg, 1);
    }
}

void Scheduler::addFaaslets(const faabric::Message& msg)
{
    auto logger = faabric::util::getLogger();
    const std::string funcStr = faabric::util::funcToString(msg, false);

    // If we've not got one faaslet per in-flight function, try to add enough
    int nFaaslets = faasletCounts[funcStr];
    int inFlightCount = inFlightCounts[funcStr];

    int difference = inFlightCount - nFaaslets;

    if (difference > 0) {
        logger->debug(
          "Scaling {} {}->{} faaslets", funcStr, nFaaslets, nFaaslets + 1);
        doAddFaaslets(msg, difference);
    }
}

void Scheduler::doAddFaaslets(const faabric::Message& msg, int count)
{
    const std::string funcStr = faabric::util::funcToString(msg, false);

    // Increment faaslet count
    faasletCounts[funcStr] += count;
    thisHostResources.set_boundexecutors(thisHostResources.boundexecutors() +
                                         count);

    // Send bind messages
    for (int i = 0; i < count; i++) {
        faabric::Message bindMsg =
          faabric::util::messageFactory(msg.user(), msg.function());
        bindMsg.set_type(faabric::Message_MessageType_BIND);
        bindMsg.set_ispython(msg.ispython());
        bindMsg.set_istypescript(msg.istypescript());
        bindMsg.set_pythonuser(msg.pythonuser());
        bindMsg.set_pythonfunction(msg.pythonfunction());
        bindMsg.set_issgx(msg.issgx());

        bindQueue->enqueue(bindMsg);
    }
}

std::string Scheduler::getThisHost()
{
    return thisHost;
}

void Scheduler::broadcastFlush()
{
    // Get all hosts
    std::unordered_set<std::string> allHosts = getAvailableHosts();

    // Remove this host from the set
    allHosts.erase(thisHost);

    // Dispatch flush message to all other hosts
    for (auto& otherHost : allHosts) {
        FunctionCallClient c(otherHost);
        c.sendFlush();
    }

    // Perform flush locally
    flushLocally();
}

void Scheduler::flushLocally()
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();
    logger->info("Flushing host {}",
                 faabric::util::getSystemConfig().endpointHost);

    // Notify all warm faaslets of flush
    for (const auto& p : faasletCounts) {
        if (p.second == 0) {
            continue;
        }

        // Clear any existing messages in the queue
        std::shared_ptr<InMemoryBatchQueue> queue = queueMap[p.first];
        queue->drain();

        // Dispatch a flush message for each warm faaslet
        std::shared_ptr<faabric::BatchExecuteRequest> req =
          faabric::util::batchExecFactory("", "", p.second);
        for (int i = 0; i < p.second; i++) {
            req->mutable_messages()->at(i).set_type(faabric::Message::FLUSH);

            std::vector<int> idxs = { i };
            queue->enqueue(std::make_pair(idxs, req));
        }
    }

    // Wait for flush messages to be consumed, then clear the queues
    for (const auto& p : queueMap) {
        logger->debug("Waiting for {} to drain on flush", p.first);
        p.second->waitToDrain(FLUSH_TIMEOUT_MS);
        p.second->reset();
    }
}

void Scheduler::setFunctionResult(faabric::Message& msg)
{
    redis::Redis& redis = redis::Redis::getQueue();

    // Record which host did the execution
    msg.set_executedhost(faabric::util::getSystemConfig().endpointHost);

    // Set finish timestamp
    msg.set_finishtimestamp(faabric::util::getGlobalClock().epochMillis());

    std::string key = msg.resultkey();
    if (key.empty()) {
        throw std::runtime_error("Result key empty. Cannot publish result");
    }

    // Write the successful result to the result queue
    std::vector<uint8_t> inputData = faabric::util::messageToBytes(msg);
    redis.enqueueBytes(key, inputData);

    // Set the result key to expire
    redis.expire(key, RESULT_KEY_EXPIRY);

    // Set long-lived result for function too
    redis.set(msg.statuskey(), inputData);
    redis.expire(key, STATUS_KEY_EXPIRY);
}

faabric::Message Scheduler::getFunctionResult(unsigned int messageId,
                                              int timeoutMs)
{
    if (messageId == 0) {
        throw std::runtime_error("Must provide non-zero message ID");
    }

    redis::Redis& redis = redis::Redis::getQueue();

    bool isBlocking = timeoutMs > 0;

    std::string resultKey = faabric::util::resultKeyFromMessageId(messageId);

    faabric::Message msgResult;

    if (isBlocking) {
        // Blocking version will throw an exception when timing out which is
        // handled by the caller.
        std::vector<uint8_t> result = redis.dequeueBytes(resultKey, timeoutMs);
        msgResult.ParseFromArray(result.data(), (int)result.size());
    } else {
        // Non-blocking version will tolerate empty responses, therefore we
        // handle the exception here
        std::vector<uint8_t> result;
        try {
            result = redis.dequeueBytes(resultKey, timeoutMs);
        } catch (redis::RedisNoResponseException& ex) {
            // Ok for no response when not blocking
        }

        if (result.empty()) {
            // Empty result has special type
            msgResult.set_type(faabric::Message_MessageType_EMPTY);
        } else {
            // Normal response if we get something from redis
            msgResult.ParseFromArray(result.data(), (int)result.size());
        }
    }

    return msgResult;
}

std::string Scheduler::getMessageStatus(unsigned int messageId)
{
    const faabric::Message result = getFunctionResult(messageId, 0);

    if (result.type() == faabric::Message_MessageType_EMPTY) {
        return "RUNNING";
    } else if (result.returnvalue() == 0) {
        return "SUCCESS: " + result.outputdata();
    } else {
        return "FAILED: " + result.outputdata();
    }
}

faabric::HostResources Scheduler::getThisHostResources()
{
    return thisHostResources;
}

void Scheduler::setThisHostResources(faabric::HostResources& res)
{
    thisHostResources = res;
}

faabric::HostResources Scheduler::getHostResources(const std::string& host)
{
    // Get the resources for that host
    faabric::ResourceRequest resourceReq;
    FunctionCallClient c(host);

    faabric::HostResources resp = c.getResources(resourceReq);

    return resp;
}

// --------------------------------------------
// EXECUTION GRAPH
// --------------------------------------------

#define CHAINED_SET_PREFIX "chained_"
std::string getChainedKey(unsigned int msgId)
{
    return std::string(CHAINED_SET_PREFIX) + std::to_string(msgId);
}

void Scheduler::logChainedFunction(unsigned int parentMessageId,
                                   unsigned int chainedMessageId)
{
    redis::Redis& redis = redis::Redis::getQueue();

    const std::string& key = getChainedKey(parentMessageId);
    redis.sadd(key, std::to_string(chainedMessageId));
    redis.expire(key, STATUS_KEY_EXPIRY);
}

std::unordered_set<unsigned int> Scheduler::getChainedFunctions(
  unsigned int msgId)
{
    redis::Redis& redis = redis::Redis::getQueue();

    const std::string& key = getChainedKey(msgId);
    const std::unordered_set<std::string> chainedCalls = redis.smembers(key);

    std::unordered_set<unsigned int> chainedIds;
    for (auto i : chainedCalls) {
        chainedIds.insert(std::stoi(i));
    }

    return chainedIds;
}

ExecGraph Scheduler::getFunctionExecGraph(unsigned int messageId)
{
    ExecGraphNode rootNode = getFunctionExecGraphNode(messageId);
    ExecGraph graph{ .rootNode = rootNode };

    return graph;
}

ExecGraphNode Scheduler::getFunctionExecGraphNode(unsigned int messageId)
{
    redis::Redis& redis = redis::Redis::getQueue();

    // Get the result for this message
    std::string statusKey = faabric::util::statusKeyFromMessageId(messageId);
    std::vector<uint8_t> messageBytes = redis.get(statusKey);
    faabric::Message result;
    result.ParseFromArray(messageBytes.data(), (int)messageBytes.size());

    // Recurse through chained calls
    std::unordered_set<unsigned int> chainedMsgIds =
      getChainedFunctions(messageId);
    std::vector<ExecGraphNode> children;
    for (auto c : chainedMsgIds) {
        children.emplace_back(getFunctionExecGraphNode(c));
    }

    // Build the node
    ExecGraphNode node{ .msg = result, .children = children };

    return node;
}
}
