// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_MEMTABLE_H_
#define STORAGE_LEVELDB_DB_MEMTABLE_H_

#include <string>
#include <set>

#include "leveldb/db_profiler.h"
#include "db/dbformat.h"
#include "db/skiplist.h"
#include "leveldb/db.h"
#include "util/arena.h"

namespace leveldb {

    class InternalKeyComparator;

    class MemTableIterator;

    class MemTable {
    public:
        // MemTables are reference counted.  The initial reference count
        // is zero and the caller must call Ref() at least once.
        explicit MemTable(const InternalKeyComparator &comparator,
                          uint32_t memtable_id,
                          DBProfiler *db_profiler);

        MemTable(const MemTable &) = delete;

        MemTable &operator=(const MemTable &) = delete;

        // Increase reference count.
        void Ref() {
            ++refs_;
        }

        uint32_t memtableid() {
            return memtable_id_;
        }

        // Drop reference count.  Delete if no more references exist.
        uint32_t Unref() {
            --refs_;
            uint32_t refs = refs_;
            assert(refs_ >= 0);
            return refs;
        }

        // Returns an estimate of the number of bytes of data in use by this
        // data structure. It is safe to call when MemTable is being modified.
        size_t ApproximateMemoryUsage();

        // Return an iterator that yields the contents of the memtable.
        //
        // The caller must ensure that the underlying MemTable remains live
        // while the returned iterator is live.  The keys returned by this
        // iterator are internal keys encoded by AppendInternalKey in the
        // db/format.{h,cc} module.
        Iterator *NewIterator(TraceType trace_type, AccessCaller caller);

        // Add an entry into memtable that maps key to value at the
        // specified sequence number and with the specified type.
        // Typically value will be empty if type==kTypeDeletion.
        void Add(SequenceNumber seq, ValueType type, const Slice &key,
                 const Slice &value);

        // If memtable contains a value for key, store it in *value and return true.
        // If memtable contains a deletion for key, store a NotFound() error
        // in *status and return true.
        // Else, return false.
        bool Get(const LookupKey &key, std::string *value, Status *s);

        FileMetaData &meta() {
            return flushed_meta_;
        }

        ~MemTable();  // Private since only Unref() should be used to delete it
    private:
        friend class MemTableIterator;

        friend class MemTableBackwardIterator;

        struct KeyComparator {
            const InternalKeyComparator comparator;

            explicit KeyComparator(const InternalKeyComparator &c) : comparator(
                    c) {}

            int operator()(const char *a, const char *b) const;
        };

        typedef SkipList<const char *, KeyComparator> Table;


//        bool delete_ = false;

        DBProfiler *db_profiler_ = nullptr;
        KeyComparator comparator_;
        int refs_;
//        std::mutex mutex_;
        uint32_t memtable_id_;
        Arena arena_;
        Table table_;
        FileMetaData flushed_meta_;
    };

    class AtomicMemTable {
    public:
        void SetMemTable(MemTable *mem);

        void SetFlushed(uint64_t l0_file_number);

        MemTable *Ref(uint64_t *l0_fn);

        void Unref();

        bool memtable_flushed_ = false;
        uint64_t l0_file_number = 0;
        std::mutex mutex;
        MemTable *memtable = nullptr;
    };

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_MEMTABLE_H_
