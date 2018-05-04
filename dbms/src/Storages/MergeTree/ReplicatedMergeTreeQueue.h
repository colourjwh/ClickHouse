#pragma once

#include <optional>

#include <Storages/MergeTree/ReplicatedMergeTreeLogEntry.h>
#include <Storages/MergeTree/ReplicatedMergeTreeMutationEntry.h>
#include <Storages/MergeTree/ActiveDataPartSet.h>
#include <Storages/MergeTree/MergeTreeData.h>

#include <Common/ZooKeeper/ZooKeeper.h>


namespace DB
{


class MergeTreeDataMerger;


class ReplicatedMergeTreeQueue
{
private:
    friend class CurrentlyExecuting;

    using StringSet = std::set<String>;

    using LogEntry = ReplicatedMergeTreeLogEntry;
    using LogEntryPtr = LogEntry::Ptr;

    using Queue = std::list<LogEntryPtr>;

    struct ByTime
    {
        bool operator()(const LogEntryPtr & lhs, const LogEntryPtr & rhs) const
        {
            return std::forward_as_tuple(lhs->create_time, lhs.get())
                 < std::forward_as_tuple(rhs->create_time, rhs.get());
        }
    };

    /// To calculate min_unprocessed_insert_time, max_processed_insert_time, for which the replica lag is calculated.
    using InsertsByTime = std::set<LogEntryPtr, ByTime>;


    MergeTreeDataFormatVersion format_version;

    String zookeeper_path;
    String replica_path;
    String logger_name;

    /** The queue of what you need to do on this line to catch up. It is taken from ZooKeeper (/replicas/me/queue/).
      * In ZK records in chronological order. Here it is not necessary.
      */
    Queue queue;

    InsertsByTime inserts_by_time;
    time_t min_unprocessed_insert_time = 0;
    time_t max_processed_insert_time = 0;

    time_t last_queue_update = 0;

    /// parts that will appear as a result of actions performed right now by background threads (these actions are not in the queue).
    /// Used to not perform other actions at the same time with these parts.
    StringSet future_parts;

    /// To access the queue, future_parts, ...
    mutable std::mutex mutex;

    /// Provides only one simultaneous call to pullLogsToQueue.
    std::mutex pull_logs_to_queue_mutex;

    /// Ensures that only one thread is simultaneously updating mutations.
    std::mutex update_mutations_mutex;

    /** What will be the set of active parts after running the entire current queue - adding new parts and performing merges.
      * Used to determine which merges have already been assigned:
      * - if there is a part in this set, then the smaller parts inside its range are not made.
      * Additionally, special elements are also added here to explicitly disallow the merge in a certain range (see disableMergesInRange).
      * This set is protected by its mutex.
      */
    ActiveDataPartSet virtual_parts;
    std::unordered_map<String, std::set<Int64>> current_inserts;
    ActiveDataPartSet next_virtual_parts;

    std::list<ReplicatedMergeTreeMutationEntry> mutations;
    std::unordered_map<String, std::map<Int64, const ReplicatedMergeTreeMutationEntry *>> mutations_by_partition;

    Logger * log = nullptr;


    /// Put a set of (already existing) parts in virtual_parts.
    void initVirtualParts(const MergeTreeData::DataParts & parts);

    /// Load (initialize) a queue from ZooKeeper (/replicas/me/queue/).
    bool load(zkutil::ZooKeeperPtr zookeeper);

    void insertUnlocked(LogEntryPtr & entry, std::optional<time_t> & min_unprocessed_insert_time_changed, std::lock_guard<std::mutex> &);

    void remove(zkutil::ZooKeeperPtr zookeeper, LogEntryPtr & entry);

    /** Can I now try this action. If not, you need to leave it in the queue and try another one.
      * Called under the main mutex.
      */
    bool shouldExecuteLogEntry(const LogEntry & entry, String & out_postpone_reason, MergeTreeDataMerger & merger, MergeTreeData & data,
        std::lock_guard<std::mutex> &) const;

    /// Return the version (block number) of the last mutation that we don't need to apply to the part
    /// (either this mutation was already applied or the part was created after the mutation).
    /// If there is no such mutation or it has already been executed and deleted, return -1.
    /// Call under the main mutex.
    Int64 getCurrentMutationVersion(const MergeTreePartInfo & part_info, std::lock_guard<std::mutex> &) const;

    /** Check that part isn't in currently generating parts and isn't covered by them.
      * Should be called under queue's mutex.
      */
    bool isNotCoveredByFuturePartsImpl(const String & new_part_name, String & out_reason, std::lock_guard<std::mutex> &) const;

    /// After removing the queue element, update the insertion times in the RAM. Running under mutex.
    /// Returns information about what times have changed - this information can be passed to updateTimesInZooKeeper.
    void updateTimesOnRemoval(const LogEntryPtr & entry,
        std::optional<time_t> & min_unprocessed_insert_time_changed,
        std::optional<time_t> & max_processed_insert_time_changed,
        std::unique_lock<std::mutex> &);

    /// Update the insertion times in ZooKeeper.
    void updateTimesInZooKeeper(zkutil::ZooKeeperPtr zookeeper,
        std::optional<time_t> min_unprocessed_insert_time_changed,
        std::optional<time_t> max_processed_insert_time_changed) const;

    /// Returns list of currently executing entries blocking execution of specified CLEAR_COLUMN command
    Queue getConflictsForClearColumnCommand(const LogEntry & entry, String * out_conflicts_description, std::lock_guard<std::mutex> &) const;

    /// Get the map: partition ID -> block numbers of inserts that are currently committing.
    std::unordered_map<String, std::set<Int64>> loadCurrentInserts(zkutil::ZooKeeperPtr & zookeeper) const;

    /// Marks the element of the queue as running.
    class CurrentlyExecuting
    {
    private:
        ReplicatedMergeTreeQueue::LogEntryPtr entry;
        ReplicatedMergeTreeQueue & queue;

        friend class ReplicatedMergeTreeQueue;

        /// Created only in the selectEntryToProcess function. It is called under mutex.
        CurrentlyExecuting(ReplicatedMergeTreeQueue::LogEntryPtr & entry, ReplicatedMergeTreeQueue & queue);

        /// In case of fetch, we determine actual part during the execution, so we need to update entry. It is called under mutex.
        static void setActualPartName(const ReplicatedMergeTreeLogEntry & entry, const String & actual_part_name,
            ReplicatedMergeTreeQueue & queue);
    public:
        ~CurrentlyExecuting();
    };

public:
    ReplicatedMergeTreeQueue(MergeTreeDataFormatVersion format_version_)
        : format_version(format_version_)
        , virtual_parts(format_version)
        , next_virtual_parts(format_version)
    {
    }

    void initialize(const String & zookeeper_path_, const String & replica_path_, const String & logger_name_,
        const MergeTreeData::DataParts & parts, zkutil::ZooKeeperPtr zookeeper);

    /** Inserts an action to the end of the queue.
      * To restore broken parts during operation.
      * Do not insert the action itself into ZK (do it yourself).
      */
    void insert(zkutil::ZooKeeperPtr zookeeper, LogEntryPtr & entry);

    /** Delete the action with the specified part (as new_part_name) from the queue.
      * Called for unreachable actions in the queue - old lost parts.
      */
    bool remove(zkutil::ZooKeeperPtr zookeeper, const String & part_name);

    /** Copy the new entries from the shared log to the queue of this replica. Set the log_pointer to the appropriate value.
      * If next_update_event != nullptr, will call this event when new entries appear in the log.
      * Returns true if new entries have been.
      */
    bool pullLogsToQueue(zkutil::ZooKeeperPtr zookeeper, zkutil::EventPtr next_update_event);

    bool updateMutations(zkutil::ZooKeeperPtr zookeeper, zkutil::EventPtr next_update_event);

    /** Remove the action from the queue with the parts covered by part_name (from ZK and from the RAM).
      * And also wait for the completion of their execution, if they are now being executed.
      */
    void removePartProducingOpsInRange(zkutil::ZooKeeperPtr zookeeper, const String & part_name);

    /** Disables future merges and fetches inside entry.new_part_name
     *  If there are currently executing merges or fetches then throws exception.
     */
    void disableMergesAndFetchesInRange(const LogEntry & entry);

    /** In the case where there are not enough parts to perform the merge in part_name
      * - move actions with merged parts to the end of the queue
      * (in order to download a already merged part from another replica).
      */
    StringSet moveSiblingPartsForMergeToEndOfQueue(const String & part_name);

    /** Select the next action to process.
      * merger is used only to check if the merges is not suspended.
      */
    using SelectedEntry = std::pair<ReplicatedMergeTreeQueue::LogEntryPtr, std::unique_ptr<CurrentlyExecuting>>;
    SelectedEntry selectEntryToProcess(MergeTreeDataMerger & merger, MergeTreeData & data);

    /** Execute `func` function to handle the action.
      * In this case, at runtime, mark the queue element as running
      *  (add into future_parts and more).
      * If there was an exception during processing, it saves it in `entry`.
      * Returns true if there were no exceptions during the processing.
      */
    bool processEntry(std::function<zkutil::ZooKeeperPtr()> get_zookeeper, LogEntryPtr & entry, const std::function<bool(LogEntryPtr &)> func);

    /// Can we merge two parts according to the queue? True if the parts are of the same mutation version,
    /// there is no merge or mutation already selected for these parts
    /// and there are no virtual parts or unfinished inserts between them.
    bool canMergeParts(const MergeTreeDataPart & left, const MergeTreeDataPart & right, String * out_reason = nullptr) const;

    bool canMutatePart(const MergeTreePartInfo & part_info, Int64 & desired_mutation_version) const;

    MutationCommands getMutationCommands(const MergeTreePartInfo & part_info, Int64 desired_mutation_version) const;

    /// Prohibit merges in the specified range.
    void disableMergesInRange(const String & part_name);

    /** Check that part isn't in currently generating parts and isn't covered by them and add it to future_parts.
      * Locks queue's mutex.
      */
    bool addFuturePartIfNotCoveredByThem(const String & part_name, const LogEntry & entry, String & reject_reason);

    /// Count the number of merges and mutations of single parts in the queue.
    size_t countMergesAndPartMutations() const;

    struct Status
    {
        UInt32 future_parts;
        UInt32 queue_size;
        UInt32 inserts_in_queue;
        UInt32 merges_in_queue;
        UInt32 mutations_in_queue;
        UInt32 queue_oldest_time;
        UInt32 inserts_oldest_time;
        UInt32 merges_oldest_time;
        UInt32 mutations_oldest_time;
        String oldest_part_to_get;
        String oldest_part_to_merge_to;
        String oldest_part_to_mutate_to;
        UInt32 last_queue_update;
    };

    /// Get information about the queue.
    Status getStatus() const;

    /// Get the data of the queue elements.
    using LogEntriesData = std::vector<ReplicatedMergeTreeLogEntryData>;
    void getEntries(LogEntriesData & res) const;

    /// Get information about the insertion times.
    void getInsertTimes(time_t & out_min_unprocessed_insert_time, time_t & out_max_processed_insert_time) const;
};


/** Convert a number to a string in the format of the suffixes of auto-incremental nodes in ZooKeeper.
  * Negative numbers are also supported - for them the name of the node looks somewhat silly
  *  and does not match any auto-incremented node in ZK.
  */
String padIndex(Int64 index);

}
