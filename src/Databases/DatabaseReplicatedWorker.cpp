#include <Databases/DatabaseReplicatedWorker.h>
#include <Databases/DatabaseReplicated.h>
#include <Interpreters/DDLTask.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int DATABASE_REPLICATION_FAILED;
    extern const int NOT_A_LEADER;
    extern const int UNFINISHED;
}

DatabaseReplicatedDDLWorker::DatabaseReplicatedDDLWorker(DatabaseReplicated * db, ContextPtr context_)
    : DDLWorker(/* pool_size */ 1, db->zookeeper_path + "/log", context_, nullptr, {}, fmt::format("DDLWorker({})", db->getDatabaseName()))
    , database(db)
{
    /// Pool size must be 1 to avoid reordering of log entries.
    /// TODO Make a dependency graph of DDL queries. It will allow to execute independent entries in parallel.
    /// We also need similar graph to load tables on server startup in order of topsort.
}

bool DatabaseReplicatedDDLWorker::initializeMainThread()
{
    while (!stop_flag)
    {
        try
        {
            auto zookeeper = getAndSetZooKeeper();
            if (database->is_readonly)
                database->tryConnectToZooKeeperAndInitDatabase(false);
            initializeReplication();
            initialized = true;
            return true;
        }
        catch (...)
        {
            tryLogCurrentException(log, fmt::format("Error on initialization of {}", database->getDatabaseName()));
            sleepForSeconds(5);
        }
    }

    return false;
}

void DatabaseReplicatedDDLWorker::shutdown()
{
    DDLWorker::shutdown();
    wait_current_task_change.notify_all();
}

void DatabaseReplicatedDDLWorker::initializeReplication()
{
    /// Check if we need to recover replica.
    /// Invariant: replica is lost if it's log_ptr value is less then max_log_ptr - logs_to_keep.

    auto zookeeper = getAndSetZooKeeper();
    String log_ptr_str = zookeeper->get(database->replica_path + "/log_ptr");
    UInt32 our_log_ptr = parse<UInt32>(log_ptr_str);
    UInt32 max_log_ptr = parse<UInt32>(zookeeper->get(database->zookeeper_path + "/max_log_ptr"));
    logs_to_keep = parse<UInt32>(zookeeper->get(database->zookeeper_path + "/logs_to_keep"));
    if (our_log_ptr == 0 || our_log_ptr + logs_to_keep < max_log_ptr)
    {
        database->recoverLostReplica(zookeeper, our_log_ptr, max_log_ptr);
        zookeeper->set(database->replica_path + "/log_ptr", toString(max_log_ptr));
        initializeLogPointer(DDLTaskBase::getLogEntryName(max_log_ptr));
    }
    else
    {
        String log_entry_name = DDLTaskBase::getLogEntryName(our_log_ptr);
        last_skipped_entry_name.emplace(log_entry_name);
        initializeLogPointer(log_entry_name);
    }
}

String DatabaseReplicatedDDLWorker::enqueueQuery(DDLLogEntry & entry)
{
    auto zookeeper = getAndSetZooKeeper();
    return enqueueQueryImpl(zookeeper, entry, database);
}

String DatabaseReplicatedDDLWorker::enqueueQueryImpl(const ZooKeeperPtr & zookeeper, DDLLogEntry & entry,
                               DatabaseReplicated * const database, bool committed)
{
    const String query_path_prefix = database->zookeeper_path + "/log/query-";

    /// We cannot create sequential node and it's ephemeral child in a single transaction, so allocate sequential number another way
    String counter_prefix = database->zookeeper_path + "/counter/cnt-";
    String counter_lock_path = database->zookeeper_path + "/counter_lock";

    String counter_path;
    size_t iters = 1000;
    while (--iters)
    {
        Coordination::Requests ops;
        ops.emplace_back(zkutil::makeCreateRequest(counter_lock_path, database->getFullReplicaName(), zkutil::CreateMode::Ephemeral));
        ops.emplace_back(zkutil::makeCreateRequest(counter_prefix, "", zkutil::CreateMode::EphemeralSequential));
        Coordination::Responses res;

        Coordination::Error code = zookeeper->tryMulti(ops, res);
        if (code == Coordination::Error::ZOK)
        {
            counter_path = dynamic_cast<const Coordination::CreateResponse &>(*res.back()).path_created;
            break;
        }
        else if (code != Coordination::Error::ZNODEEXISTS)
            zkutil::KeeperMultiException::check(code, ops, res);
    }

    if (iters == 0)
        throw Exception(ErrorCodes::UNFINISHED,
                        "Cannot enqueue query, because some replica are trying to enqueue another query. "
                        "It may happen on high queries rate or, in rare cases, after connection loss. Client should retry.");

    String node_path = query_path_prefix + counter_path.substr(counter_prefix.size());

    /// Now create task in queue
    Coordination::Requests ops;
    /// Query is not committed yet, but we have to write it into log to avoid reordering
    ops.emplace_back(zkutil::makeCreateRequest(node_path, entry.toString(), zkutil::CreateMode::Persistent));
    /// '/try' will be replaced with '/committed' or will be removed due to expired session or other error
    if (committed)
        ops.emplace_back(zkutil::makeCreateRequest(node_path + "/committed", database->getFullReplicaName(), zkutil::CreateMode::Persistent));
    else
        ops.emplace_back(zkutil::makeCreateRequest(node_path + "/try", database->getFullReplicaName(), zkutil::CreateMode::Ephemeral));
    /// We don't need it anymore
    ops.emplace_back(zkutil::makeRemoveRequest(counter_path, -1));
    /// Unlock counters
    ops.emplace_back(zkutil::makeRemoveRequest(counter_lock_path, -1));
    /// Create status dirs
    ops.emplace_back(zkutil::makeCreateRequest(node_path + "/active", "", zkutil::CreateMode::Persistent));
    ops.emplace_back(zkutil::makeCreateRequest(node_path + "/finished", "", zkutil::CreateMode::Persistent));
    zookeeper->multi(ops);


    return node_path;
}

String DatabaseReplicatedDDLWorker::tryEnqueueAndExecuteEntry(DDLLogEntry & entry, ContextPtr query_context)
{
    /// NOTE Possibly it would be better to execute initial query on the most up-to-date node,
    /// but it requires more complex logic around /try node.

    auto zookeeper = getAndSetZooKeeper();
    UInt32 our_log_ptr = getLogPointer();
    UInt32 max_log_ptr = parse<UInt32>(zookeeper->get(database->zookeeper_path + "/max_log_ptr"));

    if (our_log_ptr + database->db_settings.max_replication_lag_to_enqueue < max_log_ptr)
        throw Exception(ErrorCodes::NOT_A_LEADER, "Cannot enqueue query on this replica, "
                        "because it has replication lag of {} queries. Try other replica.", max_log_ptr - our_log_ptr);

    String entry_path = enqueueQuery(entry);
    auto try_node = zkutil::EphemeralNodeHolder::existing(entry_path + "/try", *zookeeper);
    String entry_name = entry_path.substr(entry_path.rfind('/') + 1);
    auto task = std::make_unique<DatabaseReplicatedTask>(entry_name, entry_path, database);
    task->entry = entry;
    task->parseQueryFromEntry(context);
    assert(!task->entry.query.empty());
    assert(!zookeeper->exists(task->getFinishedNodePath()));
    task->is_initial_query = true;

    LOG_DEBUG(log, "Waiting for worker thread to process all entries before {}", entry_name);
    UInt64 timeout = query_context->getSettingsRef().database_replicated_initial_query_timeout_sec;
    {
        std::unique_lock lock{mutex};
        bool processed = wait_current_task_change.wait_for(lock, std::chrono::seconds(timeout), [&]()
        {
            assert(zookeeper->expired() || current_task <= entry_name);
            return zookeeper->expired() || current_task == entry_name || stop_flag;
        });

        if (!processed)
            throw Exception(ErrorCodes::UNFINISHED, "Timeout: Cannot enqueue query on this replica, "
                            "most likely because replica is busy with previous queue entries");
    }

    if (zookeeper->expired() || stop_flag)
        throw Exception(ErrorCodes::DATABASE_REPLICATION_FAILED, "ZooKeeper session expired or replication stopped, try again");

    processTask(*task, zookeeper);

    if (!task->was_executed)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Entry {} was executed, but was not committed: code {}: {}",
            task->entry_name,
            task->execution_status.code,
            task->execution_status.message);
    }

    try_node->setAlreadyRemoved();

    return entry_path;
}

DDLTaskPtr DatabaseReplicatedDDLWorker::initAndCheckTask(const String & entry_name, String & out_reason, const ZooKeeperPtr & zookeeper)
{
    {
        std::lock_guard lock{mutex};
        if (current_task < entry_name)
        {
            current_task = entry_name;
            wait_current_task_change.notify_all();
        }
    }

    UInt32 our_log_ptr = getLogPointer();
    UInt32 entry_num = DatabaseReplicatedTask::getLogEntryNumber(entry_name);

    if (entry_num <= our_log_ptr)
    {
        out_reason = fmt::format("Task {} already executed according to log pointer {}", entry_name, our_log_ptr);
        return {};
    }

    String entry_path = fs::path(queue_dir) / entry_name;
    auto task = std::make_unique<DatabaseReplicatedTask>(entry_name, entry_path, database);

    String initiator_name;
    zkutil::EventPtr wait_committed_or_failed = std::make_shared<Poco::Event>();

    String try_node_path = fs::path(entry_path) / "try";
    if (zookeeper->tryGet(try_node_path, initiator_name, nullptr, wait_committed_or_failed))
    {
        task->is_initial_query = initiator_name == task->host_id_str;

        /// Query is not committed yet. We cannot just skip it and execute next one, because reordering may break replication.
        LOG_TRACE(log, "Waiting for initiator {} to commit or rollback entry {}", initiator_name, entry_path);
        constexpr size_t wait_time_ms = 1000;
        size_t max_iterations = database->db_settings.wait_entry_commited_timeout_sec;
        size_t iteration = 0;

        while (!wait_committed_or_failed->tryWait(wait_time_ms))
        {
            if (stop_flag)
            {
                /// We cannot return task to process and we cannot return nullptr too,
                /// because nullptr means "task should not be executed".
                /// We can only exit by exception.
                throw Exception(ErrorCodes::UNFINISHED, "Replication was stopped");
            }

            if (max_iterations <= ++iteration)
            {
                /// What can we do if initiator hangs for some reason? Seems like we can remove /try node.
                /// Initiator will fail to commit ZooKeeperMetadataTransaction (including ops for replicated table) if /try does not exist.
                /// But it's questionable.

                /// We use tryRemove(...) because multiple hosts (including initiator) may try to do it concurrently.
                auto code = zookeeper->tryRemove(try_node_path);
                if (code != Coordination::Error::ZOK && code != Coordination::Error::ZNONODE)
                    throw Coordination::Exception(code, try_node_path);

                if (!zookeeper->exists(fs::path(entry_path) / "committed"))
                {
                    out_reason = fmt::format("Entry {} was forcefully cancelled due to timeout", entry_name);
                    return {};
                }
            }
        }
    }

    if (!zookeeper->exists(fs::path(entry_path) / "committed"))
    {
        out_reason = fmt::format("Entry {} hasn't been committed", entry_name);
        return {};
    }

    if (task->is_initial_query)
    {
        assert(!zookeeper->exists(fs::path(entry_path) / "try"));
        assert(zookeeper->exists(fs::path(entry_path) / "committed") == (zookeeper->get(task->getFinishedNodePath()) == ExecutionStatus(0).serializeText()));
        out_reason = fmt::format("Entry {} has been executed as initial query", entry_name);
        return {};
    }

    String node_data;
    if (!zookeeper->tryGet(entry_path, node_data))
    {
        LOG_ERROR(log, "Cannot get log entry {}", entry_path);
        throw Exception(ErrorCodes::LOGICAL_ERROR, "should be unreachable");
    }

    task->entry.parse(node_data);

    if (task->entry.query.empty())
    {
        /// Some replica is added or removed, let's update cached cluster
        database->setCluster(database->getClusterImpl());
        out_reason = fmt::format("Entry {} is a dummy task", entry_name);
        return {};
    }

    task->parseQueryFromEntry(context);

    if (zookeeper->exists(task->getFinishedNodePath()))
    {
        out_reason = fmt::format("Task {} has been already processed", entry_name);
        return {};
    }

    return task;
}

bool DatabaseReplicatedDDLWorker::canRemoveQueueEntry(const String & entry_name, const Coordination::Stat &)
{
    UInt32 entry_number = DDLTaskBase::getLogEntryNumber(entry_name);
    UInt32 max_log_ptr = parse<UInt32>(getAndSetZooKeeper()->get(fs::path(database->zookeeper_path) / "max_log_ptr"));
    return entry_number + logs_to_keep < max_log_ptr;
}

void DatabaseReplicatedDDLWorker::initializeLogPointer(const String & processed_entry_name)
{
    updateMaxDDLEntryID(processed_entry_name);
}

UInt32 DatabaseReplicatedDDLWorker::getLogPointer() const
{
    /// NOTE it may not be equal to the log_ptr in zk:
    ///  - max_id can be equal to log_ptr - 1 due to race condition (when it's updated in zk, but not updated in memory yet)
    ///  - max_id can be greater than log_ptr, because log_ptr is not updated for failed and dummy entries
    return max_id.load();
}

}
