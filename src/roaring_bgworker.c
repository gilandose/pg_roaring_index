#include "pg_roaring_index.h"

#include "access/relation.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/*
 * Layout of BackgroundWorker.bgw_extra for a roaring merge worker.
 * Packed as two OIDs so both database and index are known at worker startup.
 */
typedef struct RoaringWorkerArgs
{
	Oid		db_oid;
	Oid		index_oid;
} RoaringWorkerArgs;

/* ----------------------------------------------------------------
 * roaring_bgworker_main
 *
 * Entry point for a dynamically spawned background worker that merges
 * the pending insert list for one roaring index.
 *
 * The worker:
 *   1. Connects to db_oid from bgw_extra.
 *   2. Opens the index by index_oid with ShareUpdateExclusiveLock
 *      (same lock level autovacuum uses).
 *   3. Calls roaring_merge_pending, which handles the case where
 *      another merge ran first (nactive == 0 → quick return).
 *   4. Exits.
 *
 * On any error (index dropped, out of disk space, etc.) the worker logs
 * the error and exits cleanly — the hard-cap fallback in
 * roaring_pending_append will trigger a synchronous merge next time the
 * pending list grows past 2 × pending_merge_threshold.
 * ---------------------------------------------------------------- */
PGDLLEXPORT void
roaring_bgworker_main(Datum main_arg)
{
	RoaringWorkerArgs args;

	/* Extract database OID and index OID from bgw_extra. */
	memcpy(&args, MyBgworkerEntry->bgw_extra, sizeof(args));

	/* Standard bgworker signal setup. */
	pqsignal(SIGHUP,  SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* Connect to the target database. */
	BackgroundWorkerInitializeConnectionByOid(args.db_oid, InvalidOid, 0);

	PG_TRY();
	{
		Relation index;

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		index = index_open(args.index_oid, ShareUpdateExclusiveLock);
		roaring_merge_pending(index);
		index_close(index, ShareUpdateExclusiveLock);

		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		/*
		 * Log the error but don't propagate — a crash here would just
		 * restart the postmaster.  The hard-cap fallback ensures the
		 * pending list is eventually drained by an inserting backend.
		 *
		 * AbortCurrentTransaction rolls back properly; CommitTransactionCommand
		 * on an aborted transaction is undefined behaviour.
		 */
		EmitErrorReport();
		FlushErrorState();
		AbortCurrentTransaction();
	}
	PG_END_TRY();

	proc_exit(0);
}

/* ----------------------------------------------------------------
 * roaring_try_spawn_merge_worker
 *
 * Attempt to spawn a one-shot dynamic bgworker to merge the pending
 * list for 'index'.  Silently does nothing if max_worker_processes is
 * exhausted — the hard-cap path in roaring_pending_append will fall
 * back to synchronous merge in that case.
 *
 * Called from roaring_pending_append after releasing all buffer locks,
 * so no locks are held at entry.
 * ---------------------------------------------------------------- */
void
roaring_try_spawn_merge_worker(Relation index)
{
	BackgroundWorker	  worker;
	BackgroundWorkerHandle *handle;
	RoaringWorkerArgs	  args;

	args.db_oid    = MyDatabaseId;
	args.index_oid = RelationGetRelid(index);

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags	  = BGWORKER_SHMEM_ACCESS |
							BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main_arg	  = (Datum) 0;
	strlcpy(worker.bgw_library_name,  "pg_roaring_index", BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "roaring_bgworker_main", BGW_MAXLEN);
	snprintf(worker.bgw_name, BGW_MAXLEN,
			 "roaring merge %u/%u", args.db_oid, args.index_oid);
	snprintf(worker.bgw_type, BGW_MAXLEN, "roaring merge");
	StaticAssertStmt(sizeof(args) <= BGW_EXTRALEN,
					 "RoaringWorkerArgs exceeds BGW_EXTRALEN");
	memcpy(worker.bgw_extra, &args, sizeof(args));
	worker.bgw_notify_pid = 0;

	if (RegisterDynamicBackgroundWorker(&worker, &handle))
		pfree(handle);	/* don't wait; hard cap will catch stragglers */
}
