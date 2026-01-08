/*
 * Mercury-based DataPlane for SST
 *
 * This data plane uses the Mercury RPC framework for remote data access.
 * Mercury provides efficient RPC and RDMA-like bulk data transfer capabilities
 * suitable for HPC environments.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atl.h>
#include <evpath.h>
#include <mercury.h>
#include <mercury_macros.h>
#include <margo.h>

#include "sst_data.h"
#include "dp_interface.h"
#include <adios2-perfstubs-interface.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

/*
 * Mercury DataPlane Structures
 *
 * Mercury_RS_Stream - Reader-side stream structure
 * Mercury_WS_Stream - Writer-side stream structure
 * Mercury_WSR_Stream - Writer-side per-reader stream structure
 */

typedef struct _Mercury_RS_Stream
{
    CManager cm;
    void *CP_Stream;
    int Rank;

    /* Mercury runtime instance */
    margo_instance_id mid;
    hg_id_t read_rpc_id;

    /* writer info */
    int WriterCohortSize;
    CP_PeerCohort PeerCohort;
    struct _MercuryWriterContactInfo *WriterContactInfo;
    hg_addr_t *WriterAddrs; /* Mercury addresses for each writer rank */
} *Mercury_RS_Stream;

typedef struct _Mercury_WSR_Stream
{
    struct _Mercury_WS_Stream *WS_Stream;
    CP_PeerCohort PeerCohort;
    int ReaderCohortSize;
    struct _MercuryReaderContactInfo *ReaderContactInfo;
    hg_addr_t *ReaderAddrs; /* Mercury addresses for each reader rank */
} *Mercury_WSR_Stream;

typedef struct _TimestepEntry
{
    size_t Timestep;
    struct _SstData *Data;
    struct _MercuryPerTimestepInfo *DP_TimestepInfo;
    struct _TimestepEntry *Next;
} *TimestepList;

typedef struct _Mercury_WS_Stream
{
    CManager cm;
    void *CP_Stream;
    int Rank;

    /* Mercury runtime instance */
    margo_instance_id mid;
    hg_id_t read_rpc_id;

    TimestepList Timesteps;

    int ReaderCount;
    Mercury_WSR_Stream *Readers;
} *Mercury_WS_Stream;

/*
 * Contact Information Structures
 *
 * These are exchanged between reader and writer sides during initialization.
 */

typedef struct _MercuryReaderContactInfo
{
    char *MercuryString; /* Mercury address string */
    void *RS_Stream;
} *MercuryReaderContactInfo;

typedef struct _MercuryWriterContactInfo
{
    char *MercuryString; /* Mercury address string */
    void *WS_Stream;
} *MercuryWriterContactInfo;

typedef struct _MercuryPerTimestepInfo
{
    char *CheckString;
    int CheckInt;
} *MercuryPerTimestepInfo;

/*
 * RPC Input/Output Structures for Mercury
 */

/* Read request RPC input */
MERCURY_GEN_PROC(read_request_in_t,
                 ((hg_uint64_t)(timestep))((hg_uint64_t)(offset))((hg_uint64_t)(length))(
                     (hg_uint64_t)(ws_stream))((hg_uint64_t)(rs_stream))((hg_int32_t)(requesting_rank)))

/* Read request RPC output */
MERCURY_GEN_PROC(read_request_out_t, ((hg_bulk_t)(bulk_handle))((hg_int32_t)(ret)))

/*
 * Mercury RPC Handler for Read Requests (Writer Side)
 *
 * This handler runs on the writer side when a reader requests data.
 */
static hg_return_t mercury_read_request_handler(hg_handle_t handle)
{
    PERFSTUBS_TIMER_START_FUNC(timer);
    hg_return_t hret;
    read_request_in_t in;
    read_request_out_t out;
    hg_bulk_t bulk_handle;
    const struct hg_info *hgi;
    margo_instance_id mid;

    /* Get input parameters */
    hret = margo_get_input(handle, &in);
    assert(hret == HG_SUCCESS);

    mid = margo_hg_handle_get_instance(handle);
    hgi = margo_get_info(handle);

    Mercury_WSR_Stream WSR_Stream = (Mercury_WSR_Stream)in.ws_stream;
    Mercury_WS_Stream WS_Stream = WSR_Stream->WS_Stream;

    /* Find the requested timestep data */
    TimestepList tmp = WS_Stream->Timesteps;
    while (tmp != NULL)
    {
        if (tmp->Timestep == in.timestep)
        {
            /* Create bulk handle for data transfer */
            void *data_ptr = (char *)tmp->Data->block + in.offset;
            hg_size_t bulk_size = (hg_size_t)in.length;
            hret = margo_bulk_create(mid, 1, &data_ptr, &bulk_size, HG_BULK_READ_ONLY,
                                     &bulk_handle);
            assert(hret == HG_SUCCESS);

            /* Return bulk handle to reader */
            out.bulk_handle = bulk_handle;
            out.ret = 0;

            hret = margo_respond(handle, &out);
            assert(hret == HG_SUCCESS);

            margo_bulk_free(bulk_handle);
            margo_free_input(handle, &in);
            margo_destroy(handle);

            PERFSTUBS_TIMER_STOP_FUNC(timer);
            return HG_SUCCESS;
        }
        tmp = tmp->Next;
    }

    /* Timestep not found */
    fprintf(stderr, "Failed to read Timestep %llu, not found\n", (unsigned long long)in.timestep);
    out.bulk_handle = HG_BULK_NULL;
    out.ret = -1;

    hret = margo_respond(handle, &out);
    margo_free_input(handle, &in);
    margo_destroy(handle);

    PERFSTUBS_TIMER_STOP_FUNC(timer);
    return HG_SUCCESS;
}
DEFINE_MARGO_RPC_HANDLER(mercury_read_request_handler)

/*
 * Completion Handle Structure
 */
typedef struct _MercuryCompletionHandle
{
    hg_handle_t handle;
    void *Buffer;
    size_t Length;
    int Rank;
    int Completed;
} *MercuryCompletionHandle;

/*
 * InitReader - Initialize reader-side data plane
 */
static DP_RS_Stream MercuryInitReader(CP_Services Svcs, void *CP_Stream,
                                      void **ReaderContactInfoPtr, struct _SstParams *Params,
                                      attr_list WriterContact, SstStats Stats)
{
    Mercury_RS_Stream Stream = malloc(sizeof(struct _Mercury_RS_Stream));
    MercuryReaderContactInfo Contact = malloc(sizeof(struct _MercuryReaderContactInfo));
    SMPI_Comm comm = Svcs->getMPIComm(CP_Stream);
    char *mercury_addr_str;
    hg_size_t addr_str_size = 256;

    memset(Stream, 0, sizeof(*Stream));
    memset(Contact, 0, sizeof(*Contact));

    Stream->CP_Stream = CP_Stream;
    SMPI_Comm_rank(comm, &Stream->Rank);

    /* Initialize Mercury/Margo
     * Using shared memory transport for local communication
     */
    Stream->mid = margo_init("na+sm", MARGO_SERVER_MODE, 1, 1);
    assert(Stream->mid != MARGO_INSTANCE_NULL);

    /* Get self address string */
    mercury_addr_str = malloc(addr_str_size);
    hg_addr_t self_addr;
    margo_addr_self(Stream->mid, &self_addr);
    margo_addr_to_string(Stream->mid, mercury_addr_str, &addr_str_size, self_addr);
    margo_addr_free(Stream->mid, self_addr);

    /* Register RPC */
    Stream->read_rpc_id =
        MARGO_REGISTER(Stream->mid, "sst_mercury_read", read_request_in_t, read_request_out_t, NULL);

    Contact->MercuryString = mercury_addr_str;
    Contact->RS_Stream = Stream;

    *ReaderContactInfoPtr = Contact;

    return Stream;
}

/*
 * DestroyReader - Cleanup reader-side resources
 */
static void MercuryDestroyReader(CP_Services Svcs, DP_RS_Stream RS_Stream_v)
{
    Mercury_RS_Stream Stream = (Mercury_RS_Stream)RS_Stream_v;

    /* Free writer addresses */
    if (Stream->WriterAddrs)
    {
        for (int i = 0; i < Stream->WriterCohortSize; i++)
        {
            if (Stream->WriterAddrs[i] != HG_ADDR_NULL)
            {
                margo_addr_free(Stream->mid, Stream->WriterAddrs[i]);
            }
        }
        free(Stream->WriterAddrs);
    }

    /* Free writer contact info strings */
    if (Stream->WriterContactInfo)
    {
        for (int i = 0; i < Stream->WriterCohortSize; i++)
        {
            if (Stream->WriterContactInfo[i].MercuryString)
            {
                free(Stream->WriterContactInfo[i].MercuryString);
            }
        }
    }

    /* Finalize Mercury - this will clean up all remaining Mercury resources */
    margo_finalize(Stream->mid);

    free(Stream);
}

/*
 * InitWriter - Initialize writer-side data plane
 */
static DP_WS_Stream MercuryInitWriter(CP_Services Svcs, void *CP_Stream, struct _SstParams *Params,
                                      attr_list DPAttrs, SstStats Stats)
{
    Mercury_WS_Stream Stream = malloc(sizeof(struct _Mercury_WS_Stream));
    SMPI_Comm comm = Svcs->getMPIComm(CP_Stream);

    memset(Stream, 0, sizeof(*Stream));

    Stream->CP_Stream = CP_Stream;
    SMPI_Comm_rank(comm, &Stream->Rank);

    /* Initialize Mercury/Margo */
    Stream->mid = margo_init("na+sm", MARGO_SERVER_MODE, 1, 1);
    assert(Stream->mid != MARGO_INSTANCE_NULL);

    /* Register RPC handler */
    Stream->read_rpc_id = MARGO_REGISTER(Stream->mid, "sst_mercury_read", read_request_in_t,
                                         read_request_out_t, mercury_read_request_handler);

    return Stream;
}

/*
 * DestroyWriter - Cleanup writer-side resources
 */
static void MercuryDestroyWriter(CP_Services Svcs, DP_WS_Stream WS_Stream_v)
{
    Mercury_WS_Stream Stream = (Mercury_WS_Stream)WS_Stream_v;

    /* Free timestep list */
    TimestepList tmp = Stream->Timesteps;
    while (tmp != NULL)
    {
        TimestepList next = tmp->Next;
        if (tmp->DP_TimestepInfo)
            free(tmp->DP_TimestepInfo);
        free(tmp);
        tmp = next;
    }

    /* Finalize Mercury */
    margo_finalize(Stream->mid);

    free(Stream);
}

/*
 * InitWriterPerReader - Initialize per-reader writer-side structures
 */
static DP_WSR_Stream MercuryInitWriterPerReader(CP_Services Svcs, DP_WS_Stream WS_Stream_v,
                                                int ReaderCohortSize, CP_PeerCohort PeerCohort,
                                                void **ProvidedReaderInfo,
                                                void **WriterContactInfoPtr)
{
    Mercury_WS_Stream WS_Stream = (Mercury_WS_Stream)WS_Stream_v;
    Mercury_WSR_Stream WSR_Stream = malloc(sizeof(struct _Mercury_WSR_Stream));
    MercuryWriterContactInfo Contact = malloc(sizeof(struct _MercuryWriterContactInfo));
    char *mercury_addr_str;
    hg_size_t addr_str_size = 256;

    memset(WSR_Stream, 0, sizeof(*WSR_Stream));

    WSR_Stream->WS_Stream = WS_Stream;
    WSR_Stream->PeerCohort = PeerCohort;
    WSR_Stream->ReaderCohortSize = ReaderCohortSize;

    /* Get self address */
    mercury_addr_str = malloc(addr_str_size);
    hg_addr_t self_addr_wsr;
    margo_addr_self(WS_Stream->mid, &self_addr_wsr);
    margo_addr_to_string(WS_Stream->mid, mercury_addr_str, &addr_str_size, self_addr_wsr);
    margo_addr_free(WS_Stream->mid, self_addr_wsr);

    Contact->MercuryString = mercury_addr_str;
    Contact->WS_Stream = WSR_Stream;

    /* Store reader addresses */
    WSR_Stream->ReaderContactInfo = (struct _MercuryReaderContactInfo *)ProvidedReaderInfo[0];
    WSR_Stream->ReaderAddrs = calloc(ReaderCohortSize, sizeof(hg_addr_t));

    for (int i = 0; i < ReaderCohortSize; i++)
    {
        MercuryReaderContactInfo reader_info = (MercuryReaderContactInfo)ProvidedReaderInfo[i];
        margo_addr_lookup(WS_Stream->mid, reader_info->MercuryString,
                          &WSR_Stream->ReaderAddrs[i]);
    }

    *WriterContactInfoPtr = Contact;

    /* Add to writer's reader list */
    WS_Stream->Readers =
        realloc(WS_Stream->Readers, sizeof(*WS_Stream->Readers) * (WS_Stream->ReaderCount + 1));
    WS_Stream->Readers[WS_Stream->ReaderCount] = WSR_Stream;
    WS_Stream->ReaderCount++;

    return WSR_Stream;
}

/*
 * DestroyWriterPerReader - Cleanup per-reader writer-side resources
 */
static void MercuryDestroyWriterPerReader(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v)
{
    Mercury_WSR_Stream WSR_Stream = (Mercury_WSR_Stream)WSR_Stream_v;

    /* Free reader addresses */
    if (WSR_Stream->ReaderAddrs)
    {
        for (int i = 0; i < WSR_Stream->ReaderCohortSize; i++)
        {
            if (WSR_Stream->ReaderAddrs[i] != HG_ADDR_NULL)
            {
                margo_addr_free(WSR_Stream->WS_Stream->mid, WSR_Stream->ReaderAddrs[i]);
            }
        }
        free(WSR_Stream->ReaderAddrs);
    }

    /* Free reader contact info strings */
    if (WSR_Stream->ReaderContactInfo)
    {
        for (int i = 0; i < WSR_Stream->ReaderCohortSize; i++)
        {
            if (WSR_Stream->ReaderContactInfo[i].MercuryString)
            {
                free(WSR_Stream->ReaderContactInfo[i].MercuryString);
            }
        }
    }

    free(WSR_Stream);
}

/*
 * ProvideWriterDataToReader - Provide writer contact info to reader
 */
static void MercuryProvideWriterDataToReader(CP_Services Svcs, DP_RS_Stream RS_Stream_v,
                                             int WriterCohortSize, CP_PeerCohort PeerCohort,
                                             void **ProvidedWriterInfo)
{
    Mercury_RS_Stream RS_Stream = (Mercury_RS_Stream)RS_Stream_v;

    RS_Stream->PeerCohort = PeerCohort;
    RS_Stream->WriterCohortSize = WriterCohortSize;
    RS_Stream->WriterContactInfo = (struct _MercuryWriterContactInfo *)ProvidedWriterInfo[0];
    RS_Stream->WriterAddrs = calloc(WriterCohortSize, sizeof(hg_addr_t));

    /* Lookup writer addresses */
    for (int i = 0; i < WriterCohortSize; i++)
    {
        MercuryWriterContactInfo writer_info = (MercuryWriterContactInfo)ProvidedWriterInfo[i];
        margo_addr_lookup(RS_Stream->mid, writer_info->MercuryString, &RS_Stream->WriterAddrs[i]);
    }
}

/*
 * ReadRemoteMemory - Initiate remote memory read using Mercury RPC
 */
static void *MercuryReadRemoteMemory(CP_Services Svcs, DP_RS_Stream RS_Stream_v, int Rank,
                                     size_t Timestep, size_t Offset, size_t Length, void *Buffer,
                                     void *DP_TimestepInfo)
{
    Mercury_RS_Stream RS_Stream = (Mercury_RS_Stream)RS_Stream_v;
    MercuryCompletionHandle Handle = malloc(sizeof(struct _MercuryCompletionHandle));
    hg_return_t hret;
    read_request_in_t in;
    read_request_out_t out;
    hg_bulk_t local_bulk_handle;
    hg_size_t hg_length = (hg_size_t)Length;

    Handle->Buffer = Buffer;
    Handle->Length = Length;
    Handle->Rank = Rank;
    Handle->Completed = 0;

    /* Create local bulk handle for receiving data */
    hret = margo_bulk_create(RS_Stream->mid, 1, &Buffer, &hg_length, HG_BULK_WRITE_ONLY,
                             &local_bulk_handle);
    assert(hret == HG_SUCCESS);

    /* Create RPC handle */
    hret = margo_create(RS_Stream->mid, RS_Stream->WriterAddrs[Rank], RS_Stream->read_rpc_id,
                        &Handle->handle);
    assert(hret == HG_SUCCESS);

    /* Set up RPC input */
    in.timestep = Timestep;
    in.offset = Offset;
    in.length = Length;
    in.ws_stream = (hg_uint64_t)RS_Stream->WriterContactInfo[Rank].WS_Stream;
    in.rs_stream = (hg_uint64_t)RS_Stream;
    in.requesting_rank = RS_Stream->Rank;

    /* Forward RPC (synchronous) */
    hret = margo_forward(Handle->handle, &in);
    assert(hret == HG_SUCCESS);

    /* Get response */
    hret = margo_get_output(Handle->handle, &out);
    assert(hret == HG_SUCCESS);

    if (out.ret == 0 && out.bulk_handle != HG_BULK_NULL)
    {
        /* Transfer data using bulk transfer */
        hret = margo_bulk_transfer(RS_Stream->mid, HG_BULK_PULL, RS_Stream->WriterAddrs[Rank],
                                    out.bulk_handle, 0, local_bulk_handle, 0, Length);
        assert(hret == HG_SUCCESS);
        Handle->Completed = 1;
    }

    margo_free_output(Handle->handle, &out);
    margo_bulk_free(local_bulk_handle);

    return Handle;
}

/*
 * WaitForCompletion - Wait for remote memory read to complete
 */
static int MercuryWaitForCompletion(CP_Services Svcs, void *Handle_v)
{
    MercuryCompletionHandle Handle = (MercuryCompletionHandle)Handle_v;

    /* In our implementation, the read is synchronous, so it's already done */
    int ret = Handle->Completed;

    margo_destroy(Handle->handle);
    free(Handle);

    return ret;
}

/*
 * NotifyConnFailure - Handle connection failure
 */
static void MercuryNotifyConnFailure(CP_Services Svcs, DP_RS_Stream RS_Stream_v,
                                     int FailedPeerRank)
{
    /* Mercury handles connection failures internally */
    /* We could mark the peer as failed and fail pending operations */
}

/*
 * ProvideTimestep - Register timestep data on writer side
 */
static void MercuryProvideTimestep(CP_Services Svcs, DP_WS_Stream WS_Stream_v,
                                   struct _SstData *Data, struct _SstData *LocalMetadata,
                                   size_t Timestep, void **TimestepInfoPtr)
{
    Mercury_WS_Stream WS_Stream = (Mercury_WS_Stream)WS_Stream_v;
    TimestepList Entry = malloc(sizeof(struct _TimestepEntry));

    Entry->Timestep = Timestep;
    Entry->Data = Data;
    Entry->DP_TimestepInfo = NULL;
    Entry->Next = WS_Stream->Timesteps;
    WS_Stream->Timesteps = Entry;

    *TimestepInfoPtr = NULL;
}

/*
 * ReleaseTimestep - Release timestep data on writer side
 */
static void MercuryReleaseTimestep(CP_Services Svcs, DP_WS_Stream WS_Stream_v, size_t Timestep)
{
    Mercury_WS_Stream WS_Stream = (Mercury_WS_Stream)WS_Stream_v;
    TimestepList List = WS_Stream->Timesteps;

    if (List && List->Timestep == Timestep)
    {
        WS_Stream->Timesteps = List->Next;
        if (List->DP_TimestepInfo)
            free(List->DP_TimestepInfo);
        free(List);
        return;
    }

    while (List != NULL && List->Next != NULL)
    {
        if (List->Next->Timestep == Timestep)
        {
            TimestepList tmp = List->Next;
            List->Next = tmp->Next;
            if (tmp->DP_TimestepInfo)
                free(tmp->DP_TimestepInfo);
            free(tmp);
            return;
        }
        List = List->Next;
    }

    fprintf(stderr, "Failed to release Timestep %zu, not found\n", Timestep);
}

/*
 * GetPriority - Return priority for this dataplane
 */
static int MercuryGetPriority(CP_Services Svcs, void *CP_Stream, struct _SstParams *Params)
{
    /* Check if Mercury is available and return appropriate priority */
    /* Higher priority means more preferred */
    /* Return -1 if Mercury cannot be used */

    /* For now, return a moderate priority if Mercury is compiled in */
    return 50; /* Medium priority */
}

/*
 * Contact Information Format Descriptions (for FFS)
 */
static FMField MercuryReaderContactList[] = {
    {"MercuryString", "string", sizeof(char *),
     FMOffset(MercuryReaderContactInfo, MercuryString)},
    {"reader_ID", "integer", sizeof(void *), FMOffset(MercuryReaderContactInfo, RS_Stream)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec MercuryReaderContactStructs[] = {
    {"MercuryReaderContactInfo", MercuryReaderContactList,
     sizeof(struct _MercuryReaderContactInfo), NULL},
    {NULL, NULL, 0, NULL}};

static FMField MercuryWriterContactList[] = {
    {"MercuryString", "string", sizeof(char *),
     FMOffset(MercuryWriterContactInfo, MercuryString)},
    {"writer_ID", "integer", sizeof(void *), FMOffset(MercuryWriterContactInfo, WS_Stream)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec MercuryWriterContactStructs[] = {
    {"MercuryWriterContactInfo", MercuryWriterContactList,
     sizeof(struct _MercuryWriterContactInfo), NULL},
    {NULL, NULL, 0, NULL}};

/*
 * LoadMercuryDP - Return the Mercury DataPlane interface
 */
static struct _CP_DP_Interface mercuryDPInterface;

extern CP_DP_Interface LoadMercuryDP()
{
    memset(&mercuryDPInterface, 0, sizeof(mercuryDPInterface));

    mercuryDPInterface.DPName = "mercury";
    mercuryDPInterface.ReaderContactFormats = MercuryReaderContactStructs;
    mercuryDPInterface.WriterContactFormats = MercuryWriterContactStructs;
    mercuryDPInterface.TimestepInfoFormats = NULL;

    mercuryDPInterface.initReader = MercuryInitReader;
    mercuryDPInterface.initWriter = MercuryInitWriter;
    mercuryDPInterface.initWriterPerReader = MercuryInitWriterPerReader;
    mercuryDPInterface.provideWriterDataToReader = MercuryProvideWriterDataToReader;

    mercuryDPInterface.readRemoteMemory = (CP_DP_ReadRemoteMemoryFunc)MercuryReadRemoteMemory;
    mercuryDPInterface.waitForCompletion = MercuryWaitForCompletion;
    mercuryDPInterface.notifyConnFailure = MercuryNotifyConnFailure;

    mercuryDPInterface.provideTimestep = (CP_DP_ProvideTimestepFunc)MercuryProvideTimestep;
    mercuryDPInterface.releaseTimestep = (CP_DP_ReleaseTimestepFunc)MercuryReleaseTimestep;

    mercuryDPInterface.destroyReader = MercuryDestroyReader;
    mercuryDPInterface.destroyWriter = MercuryDestroyWriter;
    mercuryDPInterface.destroyWriterPerReader = MercuryDestroyWriterPerReader;

    mercuryDPInterface.getPriority = MercuryGetPriority;
    mercuryDPInterface.unGetPriority = NULL;

    return &mercuryDPInterface;
}
