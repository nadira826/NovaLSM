// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// The representation of a DBImpl consists of a set of Versions.  The
// newest version is called "current".  Older versions may be kept
// around to provide a consistent view to live iterators.
//
// Each Version keeps track of a set of Table files per level.  The
// entire set of versions is maintained in a VersionSet.
//
// Version,VersionSet are thread-compatible, but require external
// synchronization on all accesses.

#ifndef STORAGE_LEVELDB_DB_VERSION_SET_H_
#define STORAGE_LEVELDB_DB_VERSION_SET_H_

#include <map>
#include <set>
#include <vector>
#include <atomic>

#include "db/dbformat.h"
#include "db/version_edit.h"
#include "port/port.h"
#include "port/thread_annotations.h"
#include "memtable.h"
#include "cc/nova_cc.h"

#define MAX_LIVE_MEMTABLES 100000

namespace leveldb {

    namespace log {
        class Writer;
    }

    class Compaction;

    class Iterator;

    class MemTable;

    class TableBuilder;

    class TableCache;

    class Version;

    class VersionSet;

    class WritableFile;

// Return the smallest index i such that files[i]->largest >= key.
// Return files.size() if there is no such file.
// REQUIRES: "files" contains a sorted list of non-overlapping files.
    int FindFile(const InternalKeyComparator &icmp,
                 const std::vector<FileMetaData *> &files, const Slice &key);

// Returns true iff some file in "files" overlaps the user key range
// [*smallest,*largest].
// smallest==nullptr represents a key smaller than all keys in the DB.
// largest==nullptr represents a key largest than all keys in the DB.
// REQUIRES: If disjoint_sorted_files, files[] contains disjoint ranges
//           in sorted order.
    bool SomeFileOverlapsRange(const InternalKeyComparator &icmp,
                               bool disjoint_sorted_files,
                               const std::vector<FileMetaData *> &files,
                               const Slice *smallest_user_key,
                               const Slice *largest_user_key);

    struct CompactionPriority {
        int level;
        double score;
    };

    enum GetSearchScope {
        kAllLevels = 0,
        kAllL0AndAllLevels = 1,
        kL1AndAbove = 2
    };

    class Version {
    public:
        // Lookup the value for key.  If found, store it in *val and
        // return OK.  Else return a non-OK status.  Fills *stats.
        // REQUIRES: lock is not held
        struct GetStats {
            FileMetaData *seek_file;
            int seek_file_level;
        };

        // Append to *iters a sequence of iterators that will
        // yield the contents of this Version when merged together.
        // REQUIRES: This version has been saved (see VersionSet::SaveTo)
        void AddIterators(const ReadOptions &, std::vector<Iterator *> *iters);

        Status Get(const ReadOptions &, const LookupKey &key, std::string *val,
                   GetStats *stats, GetSearchScope search_scope);

        Status Get(const ReadOptions &, std::vector<uint64_t> &fns,
                   const LookupKey &key,
                   std::string *val);

        // Reference count management (so Versions do not disappear out from
        // under live iterators)
        void Ref();

        uint32_t Unref();

        void GetOverlappingInputs(
                int level,
                const InternalKey *begin,  // nullptr means before all keys
                const InternalKey *end,    // nullptr means after all keys
                std::vector<FileMetaData *> *inputs,
                uint32_t limit = UINT32_MAX,
                const std::set<uint64_t> &skip_files = {},
                bool contained = false);

        // Return a human readable string that describes this version's contents.
        std::string DebugString() const;

        void QueryStats(DBStats *stats);

        void ComputeOverlappingFilesStats(
                std::map<uint64_t, FileMetaData *> *files,
                std::vector<OverlappingStats> *num_overlapping);

        bool ComputeOverlappingFilesInRange(
                std::vector<FileMetaData *> *files,
                int which,
                Compaction *compaction,
                const Slice &lower,
                const Slice &upper,
                Slice *new_lower,
                Slice *new_upper);

        void ComputeOverlappingFilesForRange(
                std::vector<FileMetaData *> *l0files,
                std::vector<FileMetaData *> *l1files,
                Compaction *compaction);

        void ComputeOverlappingFilesPerTable(
                std::map<uint64_t, FileMetaData *> *files,
                std::vector<OverlappingStats> *num_overlapping);

        void ComputeNonOverlappingSet(std::vector<Compaction *> *compactions);

        bool
        AssertNonOverlappingSet(const std::vector<Compaction *> &compactions,
                                std::string *reason);

        static std::map<uint64_t, FileMetaData *> last_fnfile;

        TableCache *table_cache();

        FileMetaData *file_meta(uint64_t fn);

        uint32_t version_id() {
            return version_id_;
        }

        void Destroy();

        explicit Version(const InternalKeyComparator *icmp,
                         TableCache *table_cache,
                         const Options *const options, uint32_t version_id)
                : icmp_(icmp),
                  table_cache_(table_cache),
                  options_(options),
                  next_(this),
                  prev_(this),
                  refs_(0),
                  file_to_compact_(nullptr),
                  file_to_compact_level_(-1), version_id_(version_id),
                  compaction_level_(-1), compaction_score_(-1) {};

        ~Version();

        // List of files per level
        std::vector<FileMetaData *> files_[config::kNumLevels];
        std::map<uint64_t, FileMetaData *> fn_files_;
        uint32_t version_id_;
    private:
        friend class Compaction;

        friend class VersionSet;

        class LevelFileNumIterator;

        Version(const Version &) = delete;

        Version &operator=(const Version &) = delete;

        void GetOverlappingInputs(
                std::vector<FileMetaData *> &inputs,
                const Slice &begin,  // nullptr means before all keys
                const Slice &end,    // nullptr means after all keys
                std::vector<FileMetaData *> *outputs,
                uint32_t limit = UINT32_MAX,
                const std::set<uint64_t> &skip_files = {},
                bool contained = false);

        void GetRange(const std::vector<FileMetaData *> &inputs,
                      Slice *smallest,
                      Slice *largest);

        void RemoveOverlapTablesWithRange(std::vector<FileMetaData *> *inputs,
                                          const Slice &smallest,
                                          const Slice &largest);

        void RemoveTables(std::vector<FileMetaData *> *inputs,
                          const std::vector<leveldb::FileMetaData *> &remove_tables);


        Iterator *
        NewConcatenatingIterator(const ReadOptions &, int level) const;

        // Call func(arg, level, f) for every file that overlaps user_key in
        // order from newest to oldest.  If an invocation of func returns
        // false, makes no more calls.
        //
        // REQUIRES: user portion of internal_key == user_key.
        void ForEachOverlapping(Slice user_key, Slice internal_key, void *arg,
                                bool (*func)(void *, int, FileMetaData *),
                                GetSearchScope search_scope);

        const InternalKeyComparator *icmp_;
        TableCache *table_cache_;
        const Options *const options_;

        Version *next_;     // Next version in linked list
        Version *prev_;     // Previous version in linked list
        int refs_ = 0;          // Number of live refs to this version


        // Next file to compact based on seek stats.
        FileMetaData *file_to_compact_;
        int file_to_compact_level_;

        // Level that should be compacted next and its compaction score.
        // Score < 1 means compaction is not strictly needed.  These fields
        // are initialized by Finalize().
        // Level that should be compacted next and its compaction score.
        // Score < 1 means compaction is not strictly needed.  These fields
        // are initialized by Finalize().
        double compaction_score_ = -1;
        int compaction_level_ = -1;
    };

    class AtomicVersion {
    public:
        void SetVersion(Version *v);

        Version *Ref();

        void Unref(const std::string &dbname);

        std::mutex mutex;
        Version *version = nullptr;
        bool deleted = false;
    };

    class VersionSet {
    public:
        VersionSet(const std::string &dbname, const Options *options,
                   TableCache *table_cache, const InternalKeyComparator *);

        VersionSet(const VersionSet &) = delete;

        VersionSet &operator=(const VersionSet &) = delete;

        ~VersionSet();

        // Apply *edit to the current version to form a new descriptor that
        // is both saved to persistent state and installed as the new
        // current version.  Will release *mu while actually writing to the file.
        // REQUIRES: *mu is held on entry.
        // REQUIRES: no other thread concurrently calls LogAndApply()
        Status LogAndApply(VersionEdit *edit, Version *new_version);

        void AppendChangesToManifest(VersionEdit *edit,
                                     NovaCCMemFile *manifest_file,
                                     uint32_t stoc_id);

        // Recover the last saved descriptor from persistent storage.
        Status Recover(Slice manifest_file,
                       std::vector<VersionSubRange> *subrange_edits);

        // Return the current version.
        Version *current() const { return current_; }

        void NewFileNumbers(uint32_t nfiles, std::queue<uint64_t> *new_fns);

        // Allocate and return a new file number
        uint64_t NewFileNumber() {
            return next_file_number_.fetch_add(1);
        }

        uint64_t NextFileNumber() {
            return next_file_number_;
        }

        // Return the number of Table files at the specified level.
        int NumLevelFiles(int level) const;

        // Return the last sequence number.
        uint64_t LastSequence() const {
            return last_sequence_.load();
        }

        // Set the last sequence number to s.
        void SetLastSequence(uint64_t s) {
            assert(s >= last_sequence_);
            last_sequence_ = s;
        }

        // Mark the specified file number as used.
        void MarkFileNumberUsed(uint64_t number);

        // Pick level and inputs for a new compaction.
        // Returns nullptr if there is no compaction to be done.
        // Otherwise returns a pointer to a heap-allocated object that
        // describes the compaction.  Caller should delete the result.
        Compaction *PickCompaction(uint32_t thread_id);

        // Create an iterator that reads over the compaction inputs for "*c".
        // The caller should delete the iterator when no longer needed.
        Iterator *MakeInputIterator(Compaction *c, EnvBGThread *bg_thread);

        void AddCompactedInputs(Compaction *c,
                                std::map<uint64_t, FileMetaData> *map);

        // Returns true iff some level needs a compaction.
        bool NeedsCompaction() const {
            Version *v = current_;
            return v->compaction_score_ >= 1;
        }

        // Add all files listed in any live version to *live.
        // May also mutate some internal state.
        void AddLiveFiles(std::set<uint64_t> *live);

        void DeleteObsoleteVersions();

        // Return the approximate offset in the database of the data for
        // "key" as of version "v".
        uint64_t ApproximateOffsetOf(Version *v, const InternalKey &key);

        uint32_t current_version_id() {
            return current_version_id_.load();
        }

        std::atomic_uint_fast64_t last_sequence_;

        AtomicMemTable mid_table_mapping_[MAX_LIVE_MEMTABLES];
        AtomicVersion versions_[MAX_LIVE_MEMTABLES];
        std::atomic_int_fast32_t version_id_seq_;

        std::mutex manifest_lock_;
    private:
        class Builder;

        friend class Compaction;

        friend class Version;

        void Finalize(Version *v);

        void GetRange(const std::vector<FileMetaData *> &inputs,
                      InternalKey *smallest,
                      InternalKey *largest);

        void GetRange2(const std::vector<FileMetaData *> &inputs1,
                       const std::vector<FileMetaData *> &inputs2,
                       InternalKey *smallest, InternalKey *largest);

        void SetupOtherInputs(Compaction *c);

        void AppendVersion(Version *v);

        Env *const env_;
        const std::string dbname_;
        const Options *const options_;
        TableCache *const table_cache_;
        const InternalKeyComparator icmp_;
        std::atomic_int_fast64_t next_file_number_;

        // Opened lazily
        WritableFile *descriptor_file_;
        log::Writer *descriptor_log_;
        Version dummy_versions_;  // Head of circular doubly-linked list of versions.
        Version *current_;        // == dummy_versions_.prev_
        std::atomic_int_fast32_t current_version_id_;

        // Per-level key at which the next compaction at that level should start.
        // Either an empty string, or a valid InternalKey.
        std::string compact_pointer_[config::kNumLevels];
    };

    // A Compaction encapsulates information about a compaction.
    class Compaction {
    public:
        std::string DebugString(const Comparator *user_comparator);

        // Return the level that is being compacted.  Inputs from "level"
        // and "level+1" will be merged to produce a set of "level+1" files.
        int level() const { return level_; }

        int target_level() const { return target_level_; }

        // Return the object that holds the edits to the descriptor done
        // by this compaction.
        VersionEdit *edit() { return &edit_; }

        // "which" must be either 0 or 1
        int num_input_files(int which) const { return inputs_[which].size(); }

        uint64_t num_input_file_sizes(int which) const {
            uint64_t size = 0;
            for (auto meta : inputs_[which]) {
                size += meta->file_size;
            }
            return size;
        }

        // Return the ith input file at "level()+which" ("which" must be 0 or 1).
        FileMetaData *
        input(int which, int i) const { return inputs_[which][i]; }

        // Maximum size of files to build during this compaction.
        uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

        // Is this a trivial compaction that can be implemented by just
        // moving a single input file to the next level (no merging or splitting)
        bool IsTrivialMove() const;

        // Add all inputs to this compaction as delete operations to *edit.
        void AddInputDeletions(VersionEdit *edit);

        // Returns true iff we should stop building the current output
        // before processing "internal_key".
        bool ShouldStopBefore(const Slice &internal_key);

        Version *input_version_;

        sem_t complete_signal_;
    private:
        friend class Version;

        friend class VersionSet;

        Compaction(Version *input_version, const InternalKeyComparator *icmp,
                   const Options *options,
                   int level, int target_level);

        int level_;
        int target_level_;
        uint64_t max_output_file_size_;
        VersionEdit edit_;
        const Options *options_;
        const InternalKeyComparator *icmp_;

        // Each compaction reads inputs from "level_" and "level_+1"
        std::vector<FileMetaData *> inputs_[2];  // The two sets of inputs

        // State used to check for number of overlapping grandparent files
        // (parent == level_ + 1, grandparent == level_ + 2)
        std::vector<FileMetaData *> grandparents_;
        size_t grandparent_index_ = 0;  // Index in grandparent_starts_
        bool seen_key_ = false;             // Some output key has been seen
        int64_t overlapped_bytes_ = 0;  // Bytes of overlap between current output
        // and grandparent files

        // State for implementing IsBaseLevelForKey

        // level_ptrs_ holds indices into input_version_->levels_: our state
        // is that we are positioned at one of the file ranges for each
        // higher level than the ones involved in this compaction (i.e. for
        // all L >= level_ + 2).
        size_t level_ptrs_[config::kNumLevels];

    };

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_SET_H
