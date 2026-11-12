// SPDX-License-Identifier: Apache-2.0
#include <device.h>
#include <block_source.h>
#include <proto.hpp>
#include <nvme/nvme.h>
#include <ioctl.h>
#include <errno.h>
#include <mutex.h>

#define NVME_CSTS_FATAL (1U << 1)
#define NVME_CSTS_RDY (1U << 0)

#define NVME_SQE_OPC_ADMIN_CREATE_IO_SQ 1U
#define NVME_SQE_OPC_ADMIN_CREATE_IO_CQ 5U
#define NVME_SQE_OPC_ADMIN_IDENTIFY 6U

#define NVME_SQE_OPC_IO_FLUSH 0U
#define NVME_SQE_OPC_IO_WRITE 1U
#define NVME_SQE_OPC_IO_READ 2U

#define NVME_ADMIN_IDENTIFY_CNS_ID_NS 0x00U
#define NVME_ADMIN_IDENTIFY_CNS_ID_CTRL 0x01U
#define NVME_ADMIN_IDENTIFY_CNS_ACT_NSL 0x02U

// static inline char *LeadingWhitespace(char *beg, char *end)
// {
//     while (end > beg && *--end <= 0x20)
//     {
//         *end = 0;
//     }
//     while (beg < end && *beg <= 0x20)
//     {
//         beg++;
//     }
//     return beg;
// }

static mutex_t nvme_operate_lock;
static bool nvme_lock_ready;

// Boot probing is single-threaded before a current task exists. Runtime I/O
// uses a sleeping lock; no DMA/PRP allocation occurs under a driver spin lock.
class NVMEOperation
{
    bool locked;
    bool valid;
public:
    NVMEOperation() : locked(nvme_lock_ready && get_current_task()), valid(true)
    {
        if (locked && mutex_lock(&nvme_operate_lock) != 0)
        {
            locked = false;
            valid = false;
        }
    }
    ~NVMEOperation() { if (locked) mutex_unlock(&nvme_operate_lock); }
    bool acquired() const { return valid; }
};

void NVMEConfigureQ(NVME_CONTROLLER *ctrl, NVME_QUEUE_COMMON *q, uint32_t idx, uint32_t len)
{
    memset(q, 0, sizeof(NVME_QUEUE_COMMON));
    q->CTR = ctrl;
    q->ID = idx >> 1;
    q->DBL = (uint32_t *)(((uint8_t *)ctrl->CAP) + 0x1000 + idx * ctrl->DST);
    q->MSK = len - 1;
}
int NVMEConfigureCQ(NVME_CONTROLLER *ctrl, NVME_COMPLETION_QUEUE *cq, uint32_t idx, uint32_t len)
{
    NVMEConfigureQ(ctrl, &cq->COM, idx, len);
    cq->CQE = 0;
    uint64_t phyAddr = 0;
    phyAddr = (uint64_t)alloc_frames(1);
    if (!phyAddr) return -ENOMEM;
    cq->CQE = (NVME_COMPLETION_QUEUE_ENTRY *)phys_to_virt(phyAddr);
    memset(cq->CQE, 0, 0x1000);
    cq->COM.HAD = 0;
    cq->COM.TAL = 0;
    cq->COM.PHA = 1;
    return 0;
}
int NVMEConfigureSQ(NVME_CONTROLLER *ctrl, NVME_SUBMISSION_QUEUE *sq, uint32_t idx, uint32_t len)
{
    NVMEConfigureQ(ctrl, &sq->COM, idx, len);
    uint64_t phyAddr = 0;
    phyAddr = (uint64_t)alloc_frames(1);
    if (!phyAddr) return -ENOMEM;
    sq->SQE = (NVME_SUBMISSION_QUEUE_ENTRY *)phys_to_virt(phyAddr);
    memset(sq->SQE, 0, 0x1000);
    sq->COM.HAD = 0;
    sq->COM.TAL = 0;
    sq->COM.PHA = 0;
    return 0;
}
// Controller enable/disable uses CAP.TO; ordinary I/O gets a bounded 30s
// deadline. These waits use the monotonic HPET clock, including during boot.
static constexpr uint64_t kCommandTimeoutNs = 30000000000ULL;

static bool nvme_status_success(uint16_t status)
{
    // Completion STS includes the phase bit in bit 0. Both SC and SCT must be
    // zero: checking SC alone can incorrectly accept a non-generic error.
    return (status & 0x0ffeU) == 0;
}

int NVMEWaitingRDY(NVME_CONTROLLER *ctrl, uint32_t ready)
{
    uint64_t start = nanoTime();
    uint64_t timeout = uint64_t(ctrl->WTO ? ctrl->WTO : 500) * 1000000ULL;
    for (;;)
    {
        uint32_t status = ctrl->CAP->CST;
        if (ready && (status & NVME_CSTS_FATAL)) return -EIO;
        if ((status & NVME_CSTS_RDY) == ready) return 0;
        if (nanoTime() - start >= timeout) return -ETIMEDOUT;
        cpu_relax();
    }
}

static bool nvme_quiesce(NVME_CONTROLLER *ctrl)
{
    ctrl->CAP->CC = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ctrl->DMA_STOPPED = NVMEWaitingRDY(ctrl, 0) == 0;
    return ctrl->DMA_STOPPED;
}

static NVME_COMPLETION_QUEUE_ENTRY nvme_transport_error(NVME_CONTROLLER *ctrl)
{
    NVME_COMPLETION_QUEUE_ENTRY error = {};
    error.STS = 0xffff;
    ctrl->FAILED = true;
    if (!nvme_quiesce(ctrl))
        write_serial_string("NVME: controller could not quiesce; DMA allocations remain pinned\n");
    return error;
}

NVME_COMPLETION_QUEUE_ENTRY NVMEWaitingCMD(NVME_SUBMISSION_QUEUE *sq, NVME_SUBMISSION_QUEUE_ENTRY *command)
{
    NVME_CONTROLLER *ctrl = sq->COM.CTR;
    NVME_COMPLETION_QUEUE_ENTRY error = {};
    error.STS = 0xffff;
    if (!ctrl || ctrl->FAILED || !sq->ICQ || !sq->SQE || !sq->ICQ->CQE) return error;
    if ((ctrl->CAP->CST & NVME_CSTS_FATAL) ||
        ((sq->COM.TAL + 1) % (sq->COM.MSK + 1ULL)) == sq->COM.HAD)
        return nvme_transport_error(ctrl);

    uint16_t id = sq->COM.TAL;
    NVME_SUBMISSION_QUEUE_ENTRY *submitted = sq->SQE + id;
    memcpy(submitted, command, sizeof(*submitted));
    submitted->CDW0 = (submitted->CDW0 & 0xffffU) | (uint32_t(id) << 16);
    sq->COM.TAL = (sq->COM.TAL + 1) % (sq->COM.MSK + 1ULL);
    // Publish the complete SQE before ringing its MMIO doorbell.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *(volatile uint32_t *)sq->COM.DBL = sq->COM.TAL;

    NVME_COMPLETION_QUEUE *cq = sq->ICQ;
    uint64_t start = nanoTime();
    while ((__atomic_load_n(&cq->CQE[cq->COM.HAD].STS, __ATOMIC_ACQUIRE) & 1U) != cq->COM.PHA)
    {
        if ((ctrl->CAP->CST & NVME_CSTS_FATAL) || nanoTime() - start >= kCommandTimeoutNs)
        {
            write_serial_string("NVME: command failed or timed out; controller taken offline\n");
            return nvme_transport_error(ctrl);
        }
        cpu_relax();
    }
    if (ctrl->CAP->CST & NVME_CSTS_FATAL) return nvme_transport_error(ctrl);
    // Snapshot before releasing the CQ slot to the controller. It may reuse
    // that memory as soon as the completion-head doorbell advances.
    NVME_COMPLETION_QUEUE_ENTRY completed;
    memcpy(&completed, cq->CQE + cq->COM.HAD, sizeof(completed));
    if (completed.CID != id || completed.QID != sq->COM.ID || completed.QHD > sq->COM.MSK)
        return nvme_transport_error(ctrl);
    sq->COM.HAD = completed.QHD;
    uint16_t next = (cq->COM.HAD + 1) % (cq->COM.MSK + 1ULL);
    if (next < cq->COM.HAD) cq->COM.PHA ^= 1;
    cq->COM.HAD = next;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *(volatile uint32_t *)cq->COM.DBL = cq->COM.HAD;
    return completed;
}

void nvme_rwfail(uint16_t status)
{
    write_serial_fmt("NVME: completion status=%x SC=%x SCT=%x\n", status,
                     (status >> 1) & 0xffU, (status >> 9) & 7U);
}

static inline void *alloc_frames_bytes(uint64_t bytes) {
    uint64_t physical = alloc_frames((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    return physical ? (void *)phys_to_virt(physical) : NULL;
}

static inline void free_frames_bytes(void *ptr, uint64_t bytes) {
    free_frames((uint64_t)virt_to_phys((uint64_t)ptr),
                (bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

bool BuildPRPList(void *vaddr, uint64_t size, NVME_PRP_LIST *prpList)
{
    if (!vaddr || !prpList || !size || size > 128 * 1024) return false;
    memset(prpList, 0, sizeof(*prpList));
    uint64_t vaddrStart = (uint64_t)vaddr;
    if (size > ~(uint64_t)0 - vaddrStart) return false;
    uint64_t vaddrEnd = vaddrStart + size;
    uint64_t pageMask = PAGE_SIZE - 1;

    uint64_t firstPage = vaddrStart & ~pageMask;
    uint64_t lastPage = (vaddrEnd - 1) & ~pageMask;
    uint32_t pageCount = ((lastPage - firstPage) / PAGE_SIZE) + 1;

    if (pageCount <= 2)
    {
        prpList->prp1 = translate_address(get_current_directory(), vaddrStart);
        if (pageCount == 1)
        {
            prpList->prp2 = 0;
        }
        else
        {
            prpList->prp2 = translate_address(get_current_directory(), firstPage + PAGE_SIZE);
        }
        prpList->UPRP = false;
        prpList->A = NULL;
        return true;
    }

    uint32_t prpEntries = pageCount - 1;
    uint64_t *prpArray = (uint64_t*)alloc_frames_bytes(prpEntries * sizeof(uint64_t));
    if (!prpArray)
        return false;

    uint64_t currentPage = firstPage + PAGE_SIZE;
    for (uint32_t i = 0; i < prpEntries; i++)
    {
        prpArray[i] = translate_address(get_current_directory(), currentPage);
        currentPage += PAGE_SIZE;
    }

    prpList->prp1 = translate_address(get_current_directory(), vaddrStart);
    prpList->prp2 = translate_address(get_current_directory(), (uint64_t)prpArray);
    prpList->UPRP = true;
    prpList->A = prpArray;
    prpList->S = prpEntries * sizeof(uint64_t);
    return true;
}

void FreePRPList(NVME_PRP_LIST *prpList)
{
    if (prpList->A)
    {
        free_frames_bytes(prpList->A, prpList->S);
        prpList->A = NULL;
    }
}

uint32_t NVMETransfer(NVME_NAMESPACE *ns, void *buf, uint64_t lba, uint32_t count, uint32_t write)
{
    NVMEOperation operation;
    if (!operation.acquired()) return 0;
    if (!count || !ns || !buf || !ns->CTRL || ns->CTRL->FAILED ||
        (write && ns->CTRL->WRITE_FAILED) || !ns->BSZ ||
        lba > ns->NLBA || count > ns->NLBA - lba) return 0;

    uint32_t transferred = 0;
    while (count)
    {
        // The existing PRP-list builder supports one list page. A small,
        // page-aligned owned buffer also prevents late DMA into caller memory
        // if a controller cannot be stopped after a command timeout.
        uint32_t chunk = 128 * 1024 / ns->BSZ;
        if (!chunk) return transferred;
        if (chunk > count) chunk = count;
        if (ns->MXRS != (uint32_t)-1 && chunk > ns->MXRS) chunk = ns->MXRS;
        if (!chunk) return transferred;
        uint64_t size = uint64_t(chunk) * ns->BSZ;
        void *bounce = alloc_frames_bytes(size);
        if (!bounce) return transferred;
        if (write) memcpy(bounce, (uint8_t *)buf + uint64_t(transferred) * ns->BSZ, size);
        NVME_PRP_LIST prp = {};
        if (!BuildPRPList(bounce, size, &prp))
        {
            free_frames_bytes(bounce, size);
            return transferred;
        }
        NVME_SUBMISSION_QUEUE_ENTRY command = {};
        command.CDW0 = write ? NVME_SQE_OPC_IO_WRITE : NVME_SQE_OPC_IO_READ;
        command.NSID = ns->NSID;
        command.CDWA = uint32_t(lba);
        command.CDWB = uint32_t(lba >> 32);
        // NLB is zero-based. Bit 31 would request Limited Retry, not FUA;
        // write durability is requested with the explicit Flush command below.
        command.CDWC = chunk - 1;
        command.DATA[0] = prp.prp1;
        command.DATA[1] = prp.prp2;
        NVME_COMPLETION_QUEUE_ENTRY completed = NVMEWaitingCMD(&ns->CTRL->ISQ, &command);
        bool success = nvme_status_success(completed.STS);
        if (success && !write)
            memcpy((uint8_t *)buf + uint64_t(transferred) * ns->BSZ, bounce, size);
        // A completion ends DMA for this command. On transport failure only
        // RDY=0 proves that it is safe to release controller-owned memory.
        if (!ns->CTRL->FAILED || ns->CTRL->DMA_STOPPED)
        {
            FreePRPList(&prp);
            free_frames_bytes(bounce, size);
        }
        if (!success)
        {
            if (write) ns->CTRL->WRITE_FAILED = true;
            nvme_rwfail(completed.STS);
            return transferred;
        }
        transferred += chunk;
        lba += chunk;
        count -= chunk;
    }
    return transferred;
}

void failed_nvme(NVME_CONTROLLER *ctrl)
{
    if (!ctrl || !nvme_quiesce(ctrl)) return;
    if (ctrl->ICQ.CQE)
    {
        free_frames((uint64_t)virt_to_phys((uint64_t)ctrl->ICQ.CQE), 1);
    }
    if (ctrl->ISQ.SQE)
    {
        free_frames((uint64_t)virt_to_phys((uint64_t)ctrl->ISQ.SQE), 1);
    }
    if (ctrl->ACQ.CQE)
    {
        free_frames((uint64_t)virt_to_phys((uint64_t)ctrl->ACQ.CQE), 1);
    }
    if (ctrl->ASQ.SQE)
    {
        free_frames((uint64_t)virt_to_phys((uint64_t)ctrl->ASQ.SQE), 1);
    }
}

void failed_namespace(NVME_IDENTIFY_NAMESPACE *identifyNS, NVME_CONTROLLER *ctrl)
{
    if (!ctrl->FAILED || ctrl->DMA_STOPPED)
        free_frames((uint64_t)virt_to_phys((uint64_t)identifyNS), 1);
}
NVME_NAMESPACE *NV_MAP[256];

size_t nvme_read(int data, void *buffer, uint64_t count, uint64_t lba)
{
    return data >= 0 && data < 256 && count <= 0xffffffffULL
               ? NVMETransfer(NV_MAP[data], buffer, lba, uint32_t(count), false) : 0;
}

size_t nvme_write(int data, void *buffer, uint64_t count, uint64_t lba)
{
    return data >= 0 && data < 256 && count <= 0xffffffffULL
               ? NVMETransfer(NV_MAP[data], buffer, lba, uint32_t(count), true) : 0;
}

static int nvme_ioctl(device_t *device, size_t request, void *)
{
    if (request != IOBLKSYNC) return -EOPNOTSUPP;
    NVMEOperation operation;
    if (!operation.acquired()) return -EIO;
    if (!device || device->vdiskid >= 256 || !NV_MAP[device->vdiskid]) return -ENODEV;
    NVME_NAMESPACE *ns = NV_MAP[device->vdiskid];
    if (ns->CTRL->FAILED || ns->CTRL->WRITE_FAILED) return -EIO;
    // NVM Flush has no data transfer, PRPs, or command-specific dwords. NSID
    // targets this namespace; this is not an invalid zero-length read/write.
    NVME_SUBMISSION_QUEUE_ENTRY command = {};
    command.CDW0 = NVME_SQE_OPC_IO_FLUSH;
    command.NSID = ns->NSID;
    NVME_COMPLETION_QUEUE_ENTRY completed = NVMEWaitingCMD(&ns->CTRL->ISQ, &command);
    if (!nvme_status_success(completed.STS))
    {
        ns->CTRL->WRITE_FAILED = true;
        nvme_rwfail(completed.STS);
        return -EIO;
    }
    return 0;
}

NVME_CONTROLLER *nvme_driver_init(uint64_t bar0, uint64_t bar_size)
{
    if (!nvme_lock_ready)
    {
        mutex_create(&nvme_operate_lock, false);
        nvme_lock_ready = true;
    }
    if (!bar0 || (bar0 & (PAGE_SIZE - 1)) || bar_size < 0x1008) return NULL;
    // Firmware may back the HHDM with huge pages. page_map_range creates 4 KiB
    // mappings and cannot split those leaves: remapping an HHDM BAR there can
    // mistake a huge physical frame for a page table. Use the dedicated driver
    // alias, also giving device registers an explicitly uncached mapping.
    NVME_CAPABILITY *cap = (NVME_CAPABILITY *)driver_phys_to_virt(bar0);
    constexpr uint64_t mmio_flags = PTE_PRESENT | PTE_WRITEABLE | (1ULL << 3) | (1ULL << 4);
    page_map_range(get_current_directory(), (uint64_t)cap, bar0, PAGE_SIZE, mmio_flags);

    if (((cap->CAP >> 48) & 0xf) != 0)
    {
        write_serial_string("NVME: controller requires pages larger than 4 KiB\n");
        return NULL;
    }
    if (!((cap->CAP >> 37) & 1))
    {
        write_serial_fmt("NVME CONTROLLER DOES NOT SUPPORT NVME COMMAND SET\n");
        return NULL;
    }
    uint64_t stride = 4ULL << ((cap->CAP >> 32) & 0xf);
    uint64_t doorbell_bytes = 3 * stride + sizeof(uint32_t);
    if (bar_size - 0x1000 < doorbell_bytes) return NULL;
    // Only queues 0 and 1 are used. A BAR may also expose a large controller
    // memory buffer, which does not need mapping for these register accesses.
    page_map_range(get_current_directory(), (uint64_t)cap + 0x1000, bar0 + 0x1000,
                   doorbell_bytes, mmio_flags);

    NVME_CONTROLLER *ctrl = (NVME_CONTROLLER *)malloc(sizeof(NVME_CONTROLLER));
    if (!ctrl) return NULL;
    memset(ctrl, 0, sizeof(NVME_CONTROLLER));
    ctrl->CAP = cap;
    ctrl->WTO = 500 * ((cap->CAP >> 24) & 0xFF);

    // RST controller
    ctrl->CAP->CC = 0;
    if (NVMEWaitingRDY(ctrl, 0))
    {
        write_serial_fmt("NVME FATAL ERROR DURING CONTROLLER SHUTDOWN\n");
        failed_nvme(ctrl);
        return NULL;
    }
    ctrl->DST = stride;

    int rc = NVMEConfigureCQ(ctrl, &ctrl->ACQ, 1, 0x1000 / sizeof(NVME_COMPLETION_QUEUE_ENTRY));
    if (rc)
    {
        failed_nvme(ctrl);
        return NULL;
    }

    rc = NVMEConfigureSQ(ctrl, &ctrl->ASQ, 0, 0x1000 / sizeof(NVME_SUBMISSION_QUEUE_ENTRY));
    if (rc)
    {
        failed_nvme(ctrl);
        return NULL;
    }
    ctrl->ASQ.ICQ = &ctrl->ACQ;

    ctrl->CAP->AQA = ((uint32_t)ctrl->ACQ.COM.MSK << 16) | ctrl->ASQ.COM.MSK;
    ctrl->CAP->ASQ = (uint64_t)virt_to_phys((uint64_t)ctrl->ASQ.SQE);
    ctrl->CAP->ACQ = (uint64_t)virt_to_phys((uint64_t)ctrl->ACQ.CQE);

    ctrl->CAP->CC = 1 | (4 << 20) | (6 << 16);
    if (NVMEWaitingRDY(ctrl, 1))
    {
        write_serial_fmt("NVME FATAL ERROR DURING CONTROLLER ENABLING");
        failed_nvme(ctrl);
        return NULL;
    }

    /* The admin queue is set up and the controller is ready. Let's figure out
       what namespaces we have. */
    // Identify Controller
    NVME_IDENTIFY_CONTROLLER *identify = 0;
    identify = (NVME_IDENTIFY_CONTROLLER *)alloc_frames(1);
    if (!identify) { failed_nvme(ctrl); return NULL; }
    identify = (NVME_IDENTIFY_CONTROLLER *)phys_to_virt((uint64_t)identify);
    memset(identify, 0, 0x1000);

    NVME_SUBMISSION_QUEUE_ENTRY sqe;
    memset(&sqe, 0, sizeof(NVME_SUBMISSION_QUEUE_ENTRY));
    sqe.CDW0 = NVME_SQE_OPC_ADMIN_IDENTIFY;
    sqe.META = 0;
    sqe.DATA[0] = translate_address(get_current_directory(), (uint64_t)identify);
    sqe.DATA[1] = 0;
    sqe.NSID = 0;
    sqe.CDWA = NVME_ADMIN_IDENTIFY_CNS_ID_CTRL;
    NVME_COMPLETION_QUEUE_ENTRY cqe = NVMEWaitingCMD(&ctrl->ASQ, &sqe);
    if (!nvme_status_success(cqe.STS))
    {
        write_serial_fmt("CANNOT IDENTIFY NVME CONTROLLER\n");
        failed_nvme(ctrl);
        return NULL;
    }

    char buf[41];
    memcpy(buf, identify->SERN, sizeof(identify->SERN));
    buf[sizeof(identify->SERN)] = 0;
    char *serialN = LeadingWhitespace(buf, buf + sizeof(identify->SERN));
    memcpy(ctrl->SER, serialN, strlen(serialN));
    // kdebug(serialN);
    // LINEFEED();
    memcpy(buf, identify->MODN, sizeof(identify->MODN));
    buf[sizeof(identify->MODN)] = 0;
    serialN = LeadingWhitespace(buf, buf + sizeof(identify->MODN));
    memcpy(ctrl->MOD, serialN, strlen(serialN));
    // kdebug(serialN);
    // LINEFEED();

    ctrl->NSC = identify->NNAM;
    uint8_t mdts = identify->MDTS;
    free_frames((uint64_t)virt_to_phys((uint64_t)identify), 1);

    if (ctrl->NSC == 0)
    {
        write_serial_fmt("NO NAMESPACE\n");
        failed_nvme(ctrl);
        return NULL;
    }

    // Create I/O Queue
    // Create I/O CQ
    {
        uint32_t qidx = 3;
        uint32_t entryCount = 1 + (ctrl->CAP->CAP & 0xFFFF);
        if (entryCount > 0x1000 / sizeof(NVME_COMPLETION_QUEUE_ENTRY))
            entryCount = 0x1000 / sizeof(NVME_COMPLETION_QUEUE_ENTRY);
        if (NVMEConfigureCQ(ctrl, &ctrl->ICQ, qidx, entryCount))
        {
            write_serial_fmt("CANNOT INIT I/O CQ\n");
            failed_nvme(ctrl);
            return NULL;
        }
        NVME_SUBMISSION_QUEUE_ENTRY ccq;
        memset(&ccq, 0, sizeof(NVME_SUBMISSION_QUEUE_ENTRY));
        ccq.CDW0 = NVME_SQE_OPC_ADMIN_CREATE_IO_CQ;
        ccq.META = 0;
        ccq.DATA[0] = (uint64_t)virt_to_phys((uint64_t)ctrl->ICQ.CQE);
        ccq.DATA[1] = 0;
        ccq.CDWA = ((uint32_t)ctrl->ICQ.COM.MSK << 16) | (qidx >> 1);
        ccq.CDWB = 1;

        cqe = NVMEWaitingCMD(&ctrl->ASQ, &ccq);
        if (!nvme_status_success(cqe.STS))
        {
            write_serial_fmt("CANNOT CREATE I/O CQ\n");
            failed_nvme(ctrl);
            return NULL;
        }
    }

    // Create I/O SQ
    {
        uint32_t qidx = 2;
        uint32_t entryCount = 1 + (ctrl->CAP->CAP & 0xFFFF);
        if (entryCount > 0x1000 / sizeof(NVME_SUBMISSION_QUEUE_ENTRY))
            entryCount = 0x1000 / sizeof(NVME_SUBMISSION_QUEUE_ENTRY);
        if (NVMEConfigureSQ(ctrl, &ctrl->ISQ, qidx, entryCount))
        {
            write_serial_fmt("CANNOT INIT I/O SQ\n");
            failed_nvme(ctrl);
            return NULL;
        }
        NVME_SUBMISSION_QUEUE_ENTRY csq;
        memset(&csq, 0, sizeof(NVME_SUBMISSION_QUEUE_ENTRY));
        csq.CDW0 = NVME_SQE_OPC_ADMIN_CREATE_IO_SQ;
        csq.META = 0;
        csq.DATA[0] = (uint64_t)virt_to_phys((uint64_t)ctrl->ISQ.SQE);
        csq.DATA[1] = 0;
        csq.CDWA = ((uint32_t)ctrl->ISQ.COM.MSK << 16) | (qidx >> 1);
        csq.CDWB = ((qidx >> 1) << 16) | 1;

        cqe = NVMEWaitingCMD(&ctrl->ASQ, &csq);
        if (!nvme_status_success(cqe.STS))
        {
            write_serial_fmt("CANNOT CREATE I/O SQ");
            failed_nvme(ctrl);
            return NULL;
        }
        ctrl->ISQ.ICQ = &ctrl->ICQ;
    }

    // /* Populate namespace IDs */
    // for (uint32_t nsidx = 0; nsidx < ctrl->NSC; nsidx++)
    // {

    uint32_t nsidx = 0;

    // Probe Namespace
    uint32_t nsid = nsidx + 1;

    NVME_IDENTIFY_NAMESPACE *identifyNS = (NVME_IDENTIFY_NAMESPACE *)alloc_frames_bytes(PAGE_SIZE);
    if (!identifyNS) { failed_nvme(ctrl); return NULL; }
    memset(identifyNS, 0, 0x1000);

    memset(&sqe, 0, sizeof(NVME_SUBMISSION_QUEUE_ENTRY));
    sqe.CDW0 = NVME_SQE_OPC_ADMIN_IDENTIFY;
    sqe.META = 0;
    sqe.DATA[0] = translate_address(get_current_directory(), (uint64_t)identifyNS);
    sqe.DATA[1] = 0;
    sqe.NSID = nsid;
    sqe.CDWA = NVME_ADMIN_IDENTIFY_CNS_ID_NS;
    cqe = NVMEWaitingCMD(&ctrl->ASQ, &sqe);
    if (!nvme_status_success(cqe.STS))
    {
        write_serial_fmt("CANNOT IDENTIFY NAMESPACE\n");
        failed_namespace(identifyNS, ctrl);
        return NULL;
    }

    uint8_t currentLBAFormat = identifyNS->FLBA & 0xF;
    if (currentLBAFormat > identifyNS->NLBA)
    {
        write_serial_fmt("Current LBA Format error\n");
        failed_namespace(identifyNS, ctrl);
        return NULL;
    }

    if (!identifyNS->SIZE)
    {
        write_serial_fmt("Invalid namespace size\n");
        failed_namespace(identifyNS, ctrl);
        return NULL;
    }

    NVME_NAMESPACE *ns = (NVME_NAMESPACE *)malloc(sizeof(NVME_NAMESPACE));
    if (!ns) { failed_namespace(identifyNS, ctrl); failed_nvme(ctrl); return NULL; }
    memset(ns, 0, sizeof(NVME_NAMESPACE));
    ns->CTRL = ctrl;
    ns->NSID = nsid;
    ns->NLBA = identifyNS->SIZE;

    NVME_LOGICAL_BLOCK_ADDRESS *fmt = identifyNS->LBAF + currentLBAFormat;

    if (fmt->DS < 9 || fmt->DS > 12 || fmt->MS)
    {
        failed_namespace(identifyNS, ctrl);
        failed_nvme(ctrl);
        free(ns);
        return NULL;
    }
    ns->BSZ = 1U << fmt->DS;
    ns->META = fmt->MS;
    if (ns->BSZ > 0x1000 || ns->NLBA > (~(uint64_t)0) / ns->BSZ)
    {
        write_serial_fmt("BLOCK SIZE > 0x1000 !!!\n");
        failed_namespace(identifyNS, ctrl);
        free(ns);
        return NULL;
    }

    ns->MXRS = 128 * 1024 / ns->BSZ;
    if (mdts && mdts < 16)
    {
        uint64_t limit = ((1ULL << mdts) * PAGE_SIZE) / ns->BSZ;
        if (limit < ns->MXRS) ns->MXRS = uint32_t(limit);
    }
    uint8_t source_eui64[8];
    memcpy(source_eui64, &identifyNS->XUID, sizeof(source_eui64));
    bool source_shared_namespace = (identifyNS->NMIC & 1) != 0;
    failed_namespace(identifyNS, ctrl);

    // regist_blkdev((char *)"nvme", ns, ns->BSZ, ns->NLBA * ns->BSZ, ns->MXRS * ns->BSZ, nvme_read, nvme_write);
    char name_buf[20];
    sprintf(name_buf, "nvme%d", 1);
    device_t sata = {};
    sata.size = ns->NLBA*ns->BSZ;
    sata.sector_size = ns->BSZ;
    sata.type = DEVICE_BLOCK;
    sata.read = (readf)nvme_read;
    sata.write = (writef)nvme_write;
    sata.flag = 1;
    sata.ioctl = nvme_ioctl;
    sata.poll = (pollf)empty;
    sata.map = (mapf)empty;
    sata.read_vbuf = NULL;
    sata.write_vbuf = NULL;
    sata.max_size   = ns->MXRS * ns->BSZ;
    strcpy(sata.drive_name, name_buf);
    int id = regist_durable_block_device(NULL, sata);
    if (id < 0 || id >= 256)
    {
        failed_nvme(ctrl);
        free(ns);
        return NULL;
    }
    NV_MAP[id] = ns;
    // Authenticate the controller that actually owns this initialized BAR.
    // Duplicate BAR aliases and shared/multipath namespaces remain unsupported.
    pci_device_t *source_controller = NULL;
    bool source_ambiguous = false;
    for (uint32_t i = 0; i < get_pci_num() && i < PCI_DEVICE_MAX; ++i)
    {
        pci_device_t *candidate = pci_devices[i];
        if (!candidate || (candidate->class_code >> 8) != 0x0108 ||
            candidate->bars[0].address != bar0 || candidate->bars[0].size != bar_size) continue;
        if (source_controller) { source_ambiguous = true; break; }
        source_controller = candidate;
    }
    if (source_controller && !source_ambiguous && !source_shared_namespace)
    {
        BlockSourceHardware provenance = {};
        provenance.controller = source_controller;
        provenance.transport = XJ_INSTALLER_SOURCE_NVME;
        provenance.block_size = ns->BSZ;
        provenance.bytes = sata.size;
        provenance.nvme_namespace = ns->NSID;
        memcpy(provenance.nvme_eui64, source_eui64, sizeof(source_eui64));
        int source_status = block_source_register((uint32_t)id, provenance);
        if (source_status) write_serial_fmt("NVMe source provenance unavailable: %d\n", source_status);
    }
    return ctrl;
}
