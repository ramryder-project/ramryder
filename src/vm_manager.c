#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include "perf_counter.h"
#include "util_common.h"
#include "vm_manager.h"
#include "guest_agent.h"
#include "memory_pool.h"
#include "util_log.h"

struct vm_instance_manager {
    int count;
    // TODO: use TAILQ instead
    struct vm_instance VMs[MAX_NUM_VM];
};

static struct vm_instance_manager g_vm_mngr;

static struct vm_instance *vm_mngr_get_instance(int vm_id)
{
    struct vm_instance *VM = NULL;

    for (int i = 0; i < MAX_NUM_VM; i++) {
        VM = &g_vm_mngr.VMs[i];
        if (VM->vm_id == vm_id && VM->initialized) {
            return VM;
        }
    }

    return NULL;
}

static int vm_mngr_get_new_memdev_idx(int vm_id)
{
    struct vm_instance *VM = NULL;

    VM = vm_mngr_get_instance(vm_id);
    if (VM == NULL) {
        LOG_ERROR("Cannot find VM %d", vm_id);
        return -1;
    }

    return VM->memdev_counter++;
}

void vm_mngr_for_each_vm(vm_handler_fn vm_handler, void *arg)
{
    struct vm_instance *VM;

    if (g_vm_mngr.count == 0) {
        return;
    }

    for (int i = 0; i < MAX_NUM_VM; i++) {
        VM = &g_vm_mngr.VMs[i];
        if (!VM->initialized) {
            continue;
        }

        vm_handler(VM, arg);
    }
}

void vm_mngr_for_each_vm_running(vm_handler_fn vm_handler, void *arg)
{
    struct vm_instance *VM;

    if (g_vm_mngr.count == 0) {
        return;
    }

    for (int i = 0; i < MAX_NUM_VM; i++) {
        VM = &g_vm_mngr.VMs[i];
        if (!VM->initialized || !VM->running) {
            continue;
        }

        vm_handler(VM, arg);
    }
}

void  vm_mngr_for_each_core(struct vm_instance *VM, core_handler_fn core_handler, void *arg)
{
    BUG_ON(VM->core_set[0] == '\0');
    char *copy = strdup(VM->core_set);
    char *token = strtok(copy, ",");

    // format: aa-bb, xx-yy
    while (token) {
        int start = -1, end = -1;

        if (sscanf(token, "%d-%d", &start, &end) == 2) {
            // core range（e.g., "20-39"）
            if (start <= end) {
                for (int i = start; i <= end; ++i) {
                    core_handler(VM, i, arg);
                }
            }
        } else if (sscanf(token, "%d", &start) == 1) {
            // single core（e.g., "41"）
            core_handler(VM, start, arg);
        }

        token = strtok(NULL, ",");
    }
    free(copy);
}

#ifdef ENABLE_PERF
static void __vm_perf_counter_setup_core(struct vm_instance *VM, int core_id,
                        void *arg __attribute__((unused)))
{
    int leader_fd = -1;

    BUG_ON(core_id < 0);
    for (int j = 0; j < PERF_EVENT_TYPE_MAX; j++) {
        if (perf_event_is_leader_arr[j]) {
            leader_fd = -1;
        }
                
        VM->perf_event_fds[j][VM->core_index] = perf_event_setup((int)core_id,
            perf_event_config_arr[j], perf_event_config1_arr[j],
            /* pin_event */ perf_event_pin_arr[j], false, false, false, false,
            /* exclude_host */ false,  /* exclude_guest */ false,
            leader_fd);
        BUG_ON(VM->perf_event_fds[j][VM->core_index] < 0);

        if (leader_fd == -1) {
            leader_fd = VM->perf_event_fds[j][VM->core_index];
        }
    }

    VM->core_index++;
}

static void vm_perf_counter_setup(struct vm_instance *VM)
{
    BUG_ON(VM->num_cores <= 0);

    // init perf counters for VM
    for (int j = 0; j < PERF_EVENT_TYPE_MAX; ++j) {
		VM->perf_event_fds[j] = (int *)malloc(VM->num_cores * sizeof(int));
		BUG_ON(VM->perf_event_fds[j] == NULL);
		VM->cum_perf_event_counts[j] = 0;
		VM->delta_perf_event_counts[j] = 0;
	}
	for (int j = 0; j < METRIC_TYPE_MAX; ++j) {
		VM->cur_metrics[j] = 0;
		VM->ewma_metrics[j] = 0;
	}
    VM->ewma_initialized = false;

    // init perf counters for each core of the VM
    VM->core_index = 0;
    vm_mngr_for_each_core(VM, __vm_perf_counter_setup_core, NULL);
}

static void vm_perf_counter_teardown(struct vm_instance *VM)
{
    uint64_t count, new_cum_count;

    for (int j = 0; j < PERF_EVENT_TYPE_MAX; ++j) {
		new_cum_count = 0;
		for (int k = 0; k < VM->num_cores; ++k) {
			count = perf_event_read(VM->perf_event_fds[j][k]);
			new_cum_count += count;
			perf_event_teardown(VM->perf_event_fds[j][k]);
		}
		if (new_cum_count < VM->cum_perf_event_counts[j]) {
			new_cum_count = VM->cum_perf_event_counts[j];
		}
		VM->delta_perf_event_counts[j] = new_cum_count - VM->cum_perf_event_counts[j];
		VM->cum_perf_event_counts[j] = new_cum_count;
		free(VM->perf_event_fds[j]);
	}
    //memset(VM->cum_perf_event_counts, 0, sizeof(VM->cum_perf_event_counts));
    //memset(VM->delta_perf_event_counts, 0, sizeof(VM->delta_perf_event_counts));
    //memset(VM->cur_metrics, 0, sizeof(VM->cur_metrics));
    //memset(VM->ewma_metrics, 0, sizeof(VM->ewma_metrics));
}
#endif

static void __vm_num_core_update(struct vm_instance *VM,
                        int core_id __attribute__((unused)),
                        void *arg __attribute__((unused)))
{
    VM->num_cores++;
}

bool vm_mngr_check_exit(int vm_id)
{
    struct vm_instance *VM = NULL;

    VM = vm_mngr_get_instance(vm_id);
    if (VM == NULL) {
        return false;
    } else {
        return true;
    }
}

int vm_mngr_instance_create(int vm_id, char *core_set)
{
    struct vm_instance *VM;

    if (g_vm_mngr.count >= MAX_NUM_VM) {
        LOG_ERROR("Cannot create more VMs");
        return -1;
    }

    // check VM existing
    if (vm_mngr_check_exit(vm_id)) {
        LOG_ERROR("Already created VM %d", vm_id);
        return -1;
    }

    // find a free enrty
    // TODO: use TAILQ instead
    for (int i = 0; i < MAX_NUM_VM; i++) {
        VM = &g_vm_mngr.VMs[i];
        if (!VM->initialized) {
            break;
        }
    }

    BUG_ON(VM->initialized);
    VM->vm_id = vm_id;
    VM->num_cores = 0;
    VM->memdev_counter = 0;
    // TODO: check core overlap with other VMs
    snprintf(VM->core_set, sizeof(VM->core_set), "%s", core_set);
    vm_mngr_for_each_core(VM, __vm_num_core_update, NULL);
    TAILQ_INIT(&VM->attached_devs);

    VM->initialized = true;
    g_vm_mngr.count++;
    LOG_INFO("VM %d created, core number: %d, coreset [%s]",
            VM->vm_id, VM->num_cores, VM->core_set);

    return 0;
}

int vm_mngr_instance_start(int vm_id)
{
    int ret;
    struct vm_instance *VM;

    VM = vm_mngr_get_instance(vm_id);
    if (VM == NULL) {
        LOG_ERROR("VM %d has not been created", vm_id);
        return -1;
    }

    if (VM->running) {
        LOG_INFO("VM %d is running", vm_id);
        return 0;
    }

    BUG_ON(!VM->initialized);
#ifdef ENABLE_PERF
    vm_perf_counter_setup(VM);
#endif
    ret = guest_agent_init(vm_id);
    if (ret < 0) {
        LOG_ERROR("Failed to init guest agent for vm %d", vm_id);
#ifdef ENABLE_PERF
        vm_perf_counter_teardown(VM);
#endif
        return -1;
    }

    VM->running = true;
    LOG_INFO("VM %d starts running", vm_id);

    return 0;
}

int vm_mngr_instance_destroy(int vm_id)
{
    struct vm_instance *VM;

    VM = vm_mngr_get_instance(vm_id);
    if (VM == NULL) {
        LOG_ERROR("VM %d has not been created", vm_id);
        return -1;
    }

    if (VM->running) {
        vm_mngr_instance_stop(vm_id);
    }

    memset(VM->core_set, 0, sizeof(VM->core_set));
    // Rlease all memroy segments allocated to this VM
    memory_pool_release_vm_memory(VM->vm_id);

    VM->initialized = false;
    g_vm_mngr.count--;
    LOG_INFO("VM %d destroyed", vm_id);

    return 0;
}

int vm_mngr_instance_stop(int vm_id)
{
    struct vm_instance *VM;

    VM = vm_mngr_get_instance(vm_id);
    if (VM == NULL) {
        LOG_ERROR("VM %d has not been created", vm_id);
        return -1;
    }

    if (!VM->running) {
        LOG_INFO("VM %d already stopped", vm_id);
        return 0;
    }

    // we must change running flag and sleep here to
    // make sure guest monitor finishes the current
    // iteration before closing perf couter fds.
    VM->running = false;
    usleep(US_PER_MS);

    guest_agent_cleanup(VM->vm_id);
#ifdef ENABLE_PERF
    vm_perf_counter_teardown(VM);
#endif

    LOG_INFO("VM %d stopped", vm_id);

    return 0;
}

int vm_mngr_instance_alloc_mem(int node_id, int vm_id,
                        int size_mb, struct vm_mem_req *vm_mem_req)
{
    int ret;
    struct vm_instance *VM;
    struct memory_dev *mem_dev;
    struct memory_request *mem_req;

    VM = vm_mngr_get_instance(vm_id);
    if (VM == NULL) {
        LOG_ERROR("VM %d has not been created", vm_id);
        return -1;
    }

    mem_dev = malloc(sizeof(*mem_dev));
    if (mem_dev == NULL) {
        LOG_ERROR("Filed to alllocate mem_dev");
        return -1;
    }

    mem_req = &mem_dev->memory_req;
    ret = memory_pool_allocate_segments(node_id, vm_id, size_mb, mem_req);
    if (ret < 0) {
        LOG_ERROR("Filed to alllocate memory from pool, node %d, vm %d, size %dMB",
            node_id, vm_id, size_mb);
        free(mem_dev);
        return -1;
    }
    mem_dev->memdev_idx = vm_mngr_get_new_memdev_idx(vm_id);
    TAILQ_INSERT_TAIL(&VM->attached_devs, mem_dev, link);

    // QEMU needs this index.
    vm_mem_req->memdev_idx = mem_dev->memdev_idx;
    snprintf(vm_mem_req->dev_path, DEV_PATH_LEN, "%s", mem_req->dev_path);
    vm_mem_req->offset_mb = mem_req->offset_mb;
    vm_mem_req->size_mb = mem_req->size_mb;
    vm_mem_req->align_mb = mem_req->align_mb;

    return 0;
}

int vm_mngr_instance_free_mem(int vm_id, int memdev_idx)
{
    int ret;
    struct vm_instance *VM;
    struct memory_dev *mem_dev;
    struct memory_request *mem_req;
    bool found = false;

    VM = vm_mngr_get_instance(vm_id);
    if (VM == NULL) {
        LOG_ERROR("VM %d has not been created", vm_id);
        return -1;
    }

    TAILQ_FOREACH(mem_dev, &VM->attached_devs, link) {
        if (mem_dev->memdev_idx == memdev_idx) {
            found = true;
            break;
        }
    }

    if (!found) {
        LOG_ERROR("Cannot find memdev %d", memdev_idx);
        return -1;
    }

    mem_req = &mem_dev->memory_req;
    ret = memory_pool_release_segments(mem_req->node_id,
                vm_id, mem_req->offset_mb, mem_req->size_mb);
    if (ret < 0) {
        LOG_ERROR("Failed to release segments from pool, VM %d, memdev %d",
            vm_id, memdev_idx);
        return -1;
    }

    // FIX: we do not decrease VM memdev counter to make sure allocation can get an unique index
    TAILQ_REMOVE(&VM->attached_devs, mem_dev, link);
    free(mem_dev);

    return 0;
}

struct memory_request *vm_mngr_instance_get_mem(int vm_id, int memdev_idx)
{
    struct vm_instance *VM;
    struct memory_dev *mem_dev;
    struct memory_request *mem_req = NULL;

    VM = vm_mngr_get_instance(vm_id);
    if (VM == NULL) {
        LOG_ERROR("VM %d has not been created", vm_id);
        return NULL;
    }

    TAILQ_FOREACH(mem_dev, &VM->attached_devs, link) {
        if (mem_dev->memdev_idx == memdev_idx) {
            mem_req = &mem_dev->memory_req;
            break;
        }
    }

    if (!mem_req) {
        LOG_ERROR("Cannot find memdev %d", memdev_idx);
        return NULL;
    }

    return mem_req;
}

int vm_mngr_dump_state(char *buffer, int buffer_size)
{
    int offset = 0;
    struct vm_instance *VM;
    struct memory_dev *mem_dev;

    if (buffer == NULL || buffer_size <= 0) {
        return -1;
    }

    offset += snprintf(buffer + offset, buffer_size - offset,
            "VM Count: %d\n", g_vm_mngr.count);
    if (offset >= buffer_size) {
        return 0;
    }

    if (g_vm_mngr.count == 0) {
        offset += snprintf(buffer + offset, buffer_size - offset, "No VM created.\n");
        return 0;
    }

    for (int i = 0; i < MAX_NUM_VM && offset < buffer_size; i++) {
        VM = &g_vm_mngr.VMs[i];
        if (!VM->initialized) {
            continue;
        }

        offset += snprintf(buffer + offset, buffer_size - offset,
                "VM id=%d cores=%d coreset=[%s] memdev_count=%d\n",
                VM->vm_id, VM->num_cores,
                VM->core_set, VM->memdev_counter);
        if (offset >= buffer_size) {
            break;
        }

        TAILQ_FOREACH(mem_dev, &VM->attached_devs, link) {
            struct memory_request *req = &mem_dev->memory_req;
            offset += snprintf(buffer + offset, buffer_size - offset,
                    "  memdev=%d node=%d path=%s size=%dMB align=%dMB offset=%dMB\n",
                    mem_dev->memdev_idx, req->node_id, req->dev_path,
                    req->size_mb, req->align_mb, req->offset_mb);
            if (offset >= buffer_size) {
                break;
            }
        }
    }

    return 0;
}

#ifdef ENABLE_PERF
void vm_mngr_update_perf_counters(void)
{
    struct vm_instance *VM;

    if (g_vm_mngr.count == 0) {
        return;
    }

    for (int i = 0; i < MAX_NUM_VM; i++) {
        VM = &g_vm_mngr.VMs[i];
        if (!VM->running) {
            continue;
        }
#ifdef ENABLE_DEBUG
    LOG_DEBUG("###### VM %d Perf Events:", VM->vm_id);
#endif
        for (int j = 0; j < PERF_EVENT_TYPE_MAX; ++j) {
			uint64_t new_cum_count = 0;
			for (int k = 0; k < VM->num_cores; ++k) {
				uint64_t count = perf_event_read(VM->perf_event_fds[j][k]);
				new_cum_count += count;
			}
			if (new_cum_count < VM->cum_perf_event_counts[j]) {
				new_cum_count = VM->cum_perf_event_counts[j];
			}
			VM->delta_perf_event_counts[j] = new_cum_count - VM->cum_perf_event_counts[j];
			VM->cum_perf_event_counts[j] = new_cum_count;
#ifdef ENABLE_DEBUG
            LOG_DEBUG("Event: %-60s: %lu", perf_event_name_arr[j], new_cum_count);
#endif
		}
    }
}

#define TSC_FREQ 2.1e9
void vm_mngr_update_metrics(int operation_window_s __attribute__((unused)))
{
    double ewma_constant = 0.2;
    struct vm_instance *VM;

    if (g_vm_mngr.count == 0) {
        return;
    }

	for (int i = 0; i < MAX_NUM_VM; i++) {
        VM = &g_vm_mngr.VMs[i];
        if (!VM->running) {
            continue;
        }

        // L3 miss latency (i.e., main memory access latency)
		VM->cur_metrics[METRIC_TYPE_LOAD_L3_MISS_LAT] =
			1e9 * ((double) VM->delta_perf_event_counts[PERF_EVENT_TYPE_OCR_OUTSTANDING_L3_MISS_DEMAND_DATA_RD]
					/ (double) VM->delta_perf_event_counts[PERF_EVENT_TYPE_OCR_L3_MISS_DEMAND_DATA_RD])
			/ ((double) VM->delta_perf_event_counts[PERF_EVENT_TYPE_CPU_CLK_UNHALTED_THREAD]
				/ (double) VM->delta_perf_event_counts[PERF_EVENT_TYPE_CPU_CLK_UNHALTED_REF_TSC]
				* TSC_FREQ);

		for (int j = 0; j < METRIC_TYPE_MAX; ++j) {
			VM->cur_metrics[j] = MIN(metric_max_arr[j], MAX(0.0, VM->cur_metrics[j]));
		}

		// Calculate EWMA
		for (int j = 0; j < METRIC_TYPE_MAX; ++j) {
			if (VM->ewma_initialized) {
				VM->ewma_metrics[j] = ewma_constant * VM->cur_metrics[j] + (1 - ewma_constant) * VM->ewma_metrics[j];
			} else {
				VM->ewma_metrics[j] = VM->cur_metrics[j];
			}
		}
		VM->ewma_initialized = true;

#ifdef ENABLE_DEBUG
        LOG_DEBUG("###### VM %d Metrics:", VM->vm_id);
        for (int j = 0; j < METRIC_TYPE_MAX; ++j) {
            LOG_DEBUG("Metric: %-60s: %f", metric_name_arr[j], VM->cur_metrics[j]);
        }
#endif
	}
}
#endif
