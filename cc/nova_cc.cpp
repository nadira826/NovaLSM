
//
// Created by Haoyu Huang on 1/11/20.
// Copyright (c) 2020 University of Southern California. All rights reserved.
//

#include "nova_cc.h"

#define MAX_BLOCK_SIZE 102400

namespace leveldb {
    NovaCCRemoteMemFile::NovaCCRemoteMemFile(Env *env, const std::string &fname,
                                             MemManager *mem_manager,
                                             DCClient *dc_client,
                                             const std::string &dbname,
                                             char *backing_mem,
                                             uint64_t thread_id,
                                             uint64_t allocated_size)
            : env_(env), fname_(fname), mem_manager_(mem_manager),
              dc_client_(dc_client),
              dbname_(dbname),
              backing_mem_(backing_mem), thread_id_(thread_id),
              allocated_size_(allocated_size),
              MemFile(nullptr, "", false) {
        EnvFileMetadata env_meta;
        env_meta.level = 0;
        RDMA_ASSERT(
                env_->NewWritableFile(fname, env_meta,
                                      &local_writable_file_).ok());
    }

    NovaCCRemoteMemFile::~NovaCCRemoteMemFile() {
        if (backing_mem_) {
            uint32_t scid = mem_manager_->slabclassid(thread_id_,
                                                      meta_.file_size);
            mem_manager_->FreeItem(thread_id_, backing_mem_, scid);
        }
        delete local_writable_file_;
    }

    Status
    NovaCCRemoteMemFile::Read(uint64_t offset, size_t n, leveldb::Slice *result,
                              char *scratch) {
        const uint64_t available = Size() - std::min(Size(), offset);
        size_t offset_ = static_cast<size_t>(offset);
        if (n > available) {
            n = static_cast<size_t>(available);
        }
        if (n == 0) {
            *result = Slice();
            return Status::OK();
        }
        if (scratch) {
            memcpy(scratch, &(backing_mem_[offset_]), n);
            *result = Slice(scratch, n);
        } else {
            *result = Slice(&(backing_mem_[offset_]), n);
        }
        return Status::OK();
    }

    Status NovaCCRemoteMemFile::Append(const leveldb::Slice &data) {
        assert(used_size_ + data.size() < allocated_size_);
        memcpy(backing_mem_ + used_size_, data.data(), data.size());
        used_size_ += data.size();
        return Status::OK();
    }

    Status
    NovaCCRemoteMemFile::Write(uint64_t offset, const leveldb::Slice &data) {
        assert(offset + data.size() < allocated_size_);
        memcpy(backing_mem_ + offset, data.data(), data.size());
        if (offset + data.size() > used_size_) {
            used_size_ = offset + data.size();
        }
        return Status::OK();
    }

    Status NovaCCRemoteMemFile::Fsync() {
        // NOOP.
        uint32_t req_id = dc_client_->InitiateFlushSSTable(dbname_,
                                                           meta_.number, meta_,
                                                           backing_mem_);
        while (!dc_client_->IsDone(req_id)) {
            //
        }
        local_writable_file_->Sync();
        local_writable_file_->Close();
    }


    NovaCCRemoteRandomAccessFile::NovaCCRemoteRandomAccessFile(
            const std::string &dbname, uint64_t file_number,
            const leveldb::FileMetaData &meta, leveldb::DCClient *dc_client,
            leveldb::MemManager *mem_manager, uint64_t thread_id,
            bool cache_all) : dbname_(
            dbname), file_number_(file_number), meta_(meta), dc_client_(
            dc_client), mem_manager_(mem_manager), thread_id_(thread_id),
                              prefetch_all_(cache_all) {

    }

    NovaCCRemoteRandomAccessFile::~NovaCCRemoteRandomAccessFile() {
        if (backing_mem_table_) {
            uint32_t scid = mem_manager_->slabclassid(thread_id_,
                                                      meta_.file_size);
            mem_manager_->FreeItem(thread_id_, backing_mem_table_, scid);
        }
        if (backing_mem_block_) {
            uint32_t scid = mem_manager_->slabclassid(thread_id_,
                                                      MAX_BLOCK_SIZE);
            mem_manager_->FreeItem(thread_id_, backing_mem_block_, scid);
        }
    }

    Status NovaCCRemoteRandomAccessFile::Read(uint64_t offset, size_t n,
                                              leveldb::Slice *result,
                                              char *scratch) {
        RDMA_ASSERT(scratch);
        if (!prefetch_all_ && backing_mem_block_ == nullptr) {
            uint32_t scid = mem_manager_->slabclassid(thread_id_,
                                                      MAX_BLOCK_SIZE);
            backing_mem_block_ = mem_manager_->ItemAlloc(thread_id_, scid);
        }

        const uint64_t available =
                meta_.file_size - std::min(meta_.file_size, offset);
        if (n > available) {
            n = static_cast<size_t>(available);
        }
        if (n == 0) {
            *result = Slice();
            return Status::OK();
        }

        char *ptr = nullptr;
        if (prefetch_all_) {
            if (!done_prefetch_all_) {
                RDMA_ASSERT(ReadAll().ok());
                done_prefetch_all_ = true;
            }
            ptr = &backing_mem_table_[offset];
        } else {
            DCBlockHandle handle = {
                    .offset = offset,
                    .size = n
            };
            ptr = backing_mem_block_;
            uint32_t req_id = dc_client_->InitiateReadBlock(dbname_,
                                                            file_number_, meta_,
                                                            handle,
                                                            ptr);
            while (!dc_client_->IsDone(req_id)) {
                // Wait until the request is complete.
            }
        }


        if (scratch) {
            memcpy(scratch, ptr, n);
            *result = Slice(scratch, n);
        } else {
            *result = Slice(ptr, n);
        }
    }

    Status NovaCCRemoteRandomAccessFile::ReadAll() {
        uint32_t scid = mem_manager_->slabclassid(thread_id_, meta_.file_size);
        backing_mem_table_ = mem_manager_->ItemAlloc(thread_id_, scid);
        uint32_t req_id = dc_client_->InitiateReadSSTable(dbname_, file_number_,
                                                          meta_,
                                                          backing_mem_table_);
        while (!dc_client_->IsDone(req_id)) {

        }
    }

    NovaCCCompactionThread::NovaCCCompactionThread(rdmaio::RdmaCtrl *rdma_ctrl)
            : rdma_ctrl_(rdma_ctrl), background_work_cv_(
            &background_work_mutex_) {
    }

    void NovaCCCompactionThread::Schedule(
            void (*background_work_function)(void *background_work_arg),
            void *background_work_arg) {
        background_work_mutex_.Lock();

        // If the queue is empty, the background thread may be waiting for work.
        if (background_work_queue_.empty()) {
            background_work_cv_.Signal();
        }

        background_work_queue_.emplace(background_work_function,
                                       background_work_arg);
        background_work_mutex_.Unlock();
    }

    bool NovaCCCompactionThread::IsInitialized() {
        mutex_.Lock();
        bool is_running = is_running_;
        mutex_.Unlock();
        return is_running;
    }

    void NovaCCCompactionThread::Start() {
        rdma_store_->Init(rdma_ctrl_);

        mutex_.Lock();
        is_running_ = true;
        mutex_.Unlock();

        std::cout << "BG thread started" << std::endl;
        while (true) {
            background_work_mutex_.Lock();

            // Wait until there is work to be done.
            while (background_work_queue_.empty()) {
                background_work_cv_.Wait();
            }

            assert(!background_work_queue_.empty());
            auto background_work_function = background_work_queue_.front().function;
            void *background_work_arg = background_work_queue_.front().arg;
            background_work_queue_.pop();

            background_work_mutex_.Unlock();
            background_work_function(background_work_arg);
        }
    }

}