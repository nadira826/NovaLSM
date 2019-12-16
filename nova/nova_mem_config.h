
//
// Created by Haoyu Huang on 2/24/19.
// Copyright (c) 2019 University of Southern California. All rights reserved.
//

#ifndef RLIB_NOVA_MEM_CONFIG_H
#define RLIB_NOVA_MEM_CONFIG_H

#include <sstream>
#include <string>
#include <fstream>

#include "rdma_ctrl.hpp"
#include "nova_common.h"

namespace nova {
    using namespace std;
    using namespace rdmaio;

    enum NovaRDMAMode {
        NORMAL = 0,
        SERVER_REDIRECT = 1,
        PROXY = 2,
    };

    enum NovaRDMAPartitionMode {
        RANGE = 0,
        HASH = 1,
        DEBUG_RDMA = 2
    };

    enum NovaCacheMode {
        WRITE_AROUND = 0,
        WRITE_THROUGH = 1
    };

    struct Fragment {
        // for range partition only.
        uint64_t key_start;
        uint64_t key_end;
        uint32_t worker_id;
        uint32_t server_id;
    };

    class NovaConfig {
    public:
        static uint64_t keyhash(char *key, uint64_t nkey) {
            uint64_t hv = 0;
            str_to_int(key, &hv, nkey);
            return hv;
        }

        static Fragment *home_fragment(uint64_t key) {
            if (config->partition_mode == NovaRDMAPartitionMode::HASH) {
                return config->fragments[key % config->nfragments];
            } else if (config->partition_mode == NovaRDMAPartitionMode::RANGE) {
                Fragment *home = nullptr;
                RDMA_ASSERT(
                        key <=
                        config->fragments[config->nfragments - 1]->key_end);
                uint32_t l = 0;
                uint32_t r = config->nfragments - 1;

                while (l <= r) {
                    uint32_t m = l + (r - l) / 2;
                    home = config->fragments[m];
                    // Check if x is present at mid
                    if (key >= home->key_start && key <= home->key_end) {
                        break;
                    }
                    // If x greater, ignore left half
                    if (home->key_end < key)
                        l = m + 1;
                        // If x is smaller, ignore right half
                    else
                        r = m - 1;
                }
                RDMA_ASSERT(home->worker_id < config->num_mem_workers);
                RDMA_ASSERT(home->server_id == config->my_server_id) << key
                                                                     << ":"
                                                                     << home->server_id
                                                                     << ":"
                                                                     << config->my_server_id;
                return home;
            }
            assert(false);
            return NULL;
        }

        void ReadFragments(const std::string &path) {
            std::string line;
            ifstream file;
            file.open(path);
            vector<Fragment *> frags;
            while (std::getline(file, line)) {
                std::istringstream iss(line);
                auto *frag = new Fragment();
                RDMA_ASSERT(
                        (iss >> frag->key_start >> frag->key_end
                             >> frag->server_id
                             >> frag->worker_id));
                frags.push_back(frag);
            }
            nfragments = static_cast<uint32_t>(frags.size());
            fragments = (Fragment **) malloc(nfragments * sizeof(Fragment *));
            for (int i = 0; i < nfragments; i++) {
                fragments[i] = frags[i];
            }
            RDMA_LOG(INFO) << "Configuration has a total of " << frags.size()
                           << " fragments.";
            for (int i = 0; i < nfragments; i++) {
                RDMA_LOG(DEBUG) << "frag[" << i << "]: "
                                << fragments[i]->key_start
                                << "-" << fragments[i]->key_end
                                << "-" << fragments[i]->server_id
                                << "-" << fragments[i]->worker_id;
            }
        }

        uint32_t bucket_size() {
            return IndexEntry::size() * nindex_entry_per_bucket;
        }

        void ComputeNumberOfBuckets() {
            uint64_t index_size =
                    NovaConfig::config->index_size_mb * 1024 * 1024;
            uint64_t main_bucket_mem_size =
                    static_cast<uint64_t>(index_size / 100) *
                    NovaConfig::config->main_bucket_mem_percent;
            nbuckets = static_cast<uint32_t>(main_bucket_mem_size /
                                             bucket_size());

        }

        string to_string() {
            char output[5000];
            sprintf(output,
                    "rdma_port=[%d], mem_stores=[%d], max_msg_size=[%d], "
                    "max_num_reads=[%d], max_num_sends=[%d], "
                    "doorbell_batch=[%d], my_server_id=[%d], recordcount=[%d], "
                    "mode=[%d], partition_mode=[%d], "
                    "ingest_batch_size=[%d], value_size=[%lu], "
                    "enable_load=[%d], enable_rdma=[%d], cache_size_gb=[%lu], index_size_mb=[%lu]",
                    rdma_port, num_mem_workers, max_msg_size,
                    rdma_max_num_reads, rdma_max_num_sends,
                    rdma_doorbell_batch_size,
                    my_server_id, recordcount, mode, partition_mode,
                    rdma_pq_batch_size, load_default_value_size,
                    enable_load_data, enable_rdma, cache_size_gb,
                    index_size_mb);
            return string(output);
        }

        bool enable_load_data;
        bool enable_rdma;

        vector<Host> servers;
        int num_mem_workers;
        int my_server_id;
        int recordcount;
        uint64_t load_default_value_size;
        char *nova_buf;
        uint64_t nnovabuf;
        uint32_t nfragments;
        uint64_t cache_size_gb;
        Fragment **fragments;
        int max_msg_size;
        NovaCacheMode cache_mode;

        // Index.
        uint64_t index_buf_offset;
        uint64_t index_size_mb;
        uint32_t nindex_entry_per_bucket;
        uint32_t main_bucket_mem_percent;
        // Computed.
        uint64_t nbuckets;

        // location_cache.
        uint64_t lc_buf_offset;
        uint64_t lc_size_mb;
        uint32_t lc_nindex_entry_per_bucket;
        uint32_t lc_main_bucket_mem_percent;

        // Data.
        uint64_t data_buf_offset;

        // LevelDB.
        std::string db_path;
        std::string profiler_file_path;
        bool fsync;

        NovaRDMAMode mode;
        NovaRDMAPartitionMode partition_mode;
        int rdma_port;
        int rdma_pq_batch_size;
        int rdma_max_num_reads;
        int rdma_max_num_sends;
        int rdma_doorbell_batch_size;
        uint32_t rdma_number_of_get_retries;

        static NovaConfig *config;
        static RdmaCtrl *rdma_ctrl;
    };
}
#endif //RLIB_NOVA_MEM_CONFIG_H