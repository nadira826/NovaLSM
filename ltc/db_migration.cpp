
//
// Created by Haoyu Huang on 6/18/20.
// Copyright (c) 2020 University of Southern California. All rights reserved.
//

#include "db_migration.h"

#include "db_helper.h"
#include "db/db_impl.h"
#include "log/log_recovery.h"
#include "ltc/compaction_thread.h"

namespace nova {
    DBMigration::DBMigration(leveldb::MemManager *mem_manager,
                             leveldb::StoCBlockClient *client,
                             leveldb::StocPersistentFileManager *stoc_file_manager,
                             const std::vector<RDMAMsgHandler *> &bg_rdma_msg_handlers,
                             const std::vector<leveldb::EnvBGThread *> &bg_compaction_threads,
                             const std::vector<leveldb::EnvBGThread *> &bg_flush_memtable_threads)
            :
            mem_manager_(mem_manager),
            client_(client),
            stoc_file_manager_(stoc_file_manager),
            bg_rdma_msg_handlers_(bg_rdma_msg_handlers),
            bg_compaction_threads_(bg_compaction_threads),
            bg_flush_memtable_threads_(bg_flush_memtable_threads) {
        sem_init(&sem_, 0, 0);
    }

    void DBMigration::AddSourceMigrateDB(const std::vector<nova::LTCFragment *> &frags) {
        mu.lock();
        for (auto frag: frags) {
            DBMeta meta = {};
            meta.migrate_type = MigrateType::SOURCE;
            meta.source_fragment = frag;
            db_metas.push_back(meta);
        }
        mu.unlock();
        sem_post(&sem_);
    }

    void DBMigration::AddDestMigrateDB(char *buf, uint32_t msg_size) {
        mu.lock();
        DBMeta meta = {};
        meta.migrate_type = MigrateType::DESTINATION;
        meta.source_fragment = nullptr;
        meta.buf = buf;
        meta.msg_size = msg_size;
        db_metas.push_back(meta);
        mu.unlock();
        sem_post(&sem_);
    }

    void DBMigration::Start() {
        while (true) {
            sem_wait(&sem_);

            mu.lock();
            std::vector<DBMeta> rdbs = db_metas;
            db_metas.clear();
            mu.unlock();
            uint32_t cfg_id = nova::NovaConfig::config->current_cfg_id;

            NOVA_ASSERT(cfg_id == 1);

            std::vector<nova::LTCFragment *> source_migrates;
            std::vector<DBMeta> dest_migrates;

            for (auto dbmeta : rdbs) {
                if (dbmeta.migrate_type == MigrateType::SOURCE) {
                    source_migrates.push_back(dbmeta.source_fragment);
                } else {
                    dest_migrates.push_back(dbmeta);
                }
            }

            if (!source_migrates.empty()) {
                MigrateDB(source_migrates);
            }
            for (auto dbmeta : dest_migrates) {
                RecoverDBMeta(dbmeta, cfg_id);
            }
        }
    }

    void DBMigration::MigrateDB(const std::vector<nova::LTCFragment *> &migrate_frags) {
        // bump up the configuration id.
        std::vector<char *> bufs;
        std::vector<uint32_t> msg_sizes;
        leveldb::DBImpl *db = reinterpret_cast<leveldb::DBImpl *>(migrate_frags[0]->db);
        uint32_t scid = mem_manager_->slabclassid(0,
                                                  db->options_.max_stoc_file_size);
        for (auto frag : migrate_frags) {
            leveldb::DBImpl *db = reinterpret_cast<leveldb::DBImpl *>(frag->db);
            char *buf = mem_manager_->ItemAlloc(0, scid);
            msg_sizes.push_back(db->EncodeDBMetadata(buf));
            bufs.push_back(buf);
        }

        // Inform the destination of the buf offset.
        for (int i = 0; i < migrate_frags.size(); i++) {
            client_->InitiateRDMAWRITE(migrate_frags[i]->ltc_server_id, bufs[i],
                                       msg_sizes[i]);
        }
        for (int i = 0; i < bufs.size(); i++) {
            client_->Wait();
        }
        for (int i = 0; i < bufs.size(); i++) {
            mem_manager_->FreeItem(0, bufs[i], scid);
        }
    }

    void
    DBMigration::RecoverDBMeta(DBMeta dbmeta, int cfg_id) {
        // Open this new database;
        // Wait for lsm tree metadata.
        // build lsm tree.
        // now accept request.
        NOVA_ASSERT(dbmeta.buf);
        NOVA_ASSERT(dbmeta.buf[0] == leveldb::StoCRequestType::LTC_MIGRATION);
        char *charbuf = dbmeta.buf;
        charbuf += 1;
        leveldb::Slice buf(charbuf, nova::NovaConfig::config->max_stoc_file_size);

        uint32_t dbindex;
        uint32_t version_size;
        uint32_t srs_size;
        uint32_t memtable_size;
        uint32_t lookup_index_size;
        uint32_t tableid_mapping_size;
        uint64_t last_sequence = 0;
        uint64_t next_file_number = 0;
        NOVA_ASSERT(DecodeFixed32(&buf, &dbindex));
        NOVA_ASSERT(DecodeFixed32(&buf, &version_size));
        NOVA_ASSERT(DecodeFixed32(&buf, &srs_size));
        NOVA_ASSERT(DecodeFixed32(&buf, &memtable_size));
        NOVA_ASSERT(DecodeFixed32(&buf, &lookup_index_size));
        NOVA_ASSERT(DecodeFixed32(&buf, &tableid_mapping_size));
        NOVA_ASSERT(DecodeFixed64(&buf, &last_sequence));
        NOVA_ASSERT(DecodeFixed64(&buf, &next_file_number));

        auto reorg = new leveldb::LTCCompactionThread(mem_manager_);
        auto coord = new leveldb::LTCCompactionThread(mem_manager_);
        auto client = new leveldb::StoCBlockClient(dbindex,
                                                   stoc_file_manager_);
        auto dbint = CreateDatabase(cfg_id, dbindex, nullptr, nullptr,
                                    mem_manager_, client,
                                    bg_compaction_threads_,
                                    bg_flush_memtable_threads_, reorg,
                                    coord);
        coord->db_ = dbint;
        coord->stoc_client_ = new leveldb::StoCBlockClient(dbindex,
                                                           stoc_file_manager_);
        coord->stoc_client_->rdma_msg_handlers_ = bg_rdma_msg_handlers_;
        coord->thread_id_ = dbindex;
        auto frag = nova::NovaConfig::config->cfgs[cfg_id]->fragments[dbindex];
        frag->db = dbint;
        auto db = reinterpret_cast<leveldb::DBImpl *>(dbint);
        db->processed_writes_ = 0;
        db->number_of_puts_no_wait_ = 0;
        db->number_of_puts_wait_ = 0;
        db->number_of_steals_ = 0;
        db->number_of_wait_due_to_contention_ = 0;
        db->number_of_gets_ = 0;
        db->number_of_memtable_hits_ = 0;

        auto memtables_to_recover = db->RecoverDBMetadata(&buf, last_sequence, next_file_number);
        leveldb::LogRecovery recover(mem_manager_, client_);
        recover.Recover(memtables_to_recover, cfg_id, dbindex);

        db->StartCompaction();
        frag->is_ready_ = true;
        frag->is_ready_signal_.SignalAll();
        uint32_t scid = mem_manager_->slabclassid(0, dbmeta.msg_size);
        mem_manager_->FreeItem(0, dbmeta.buf, scid);
    }
}