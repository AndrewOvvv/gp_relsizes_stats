#include "postgres.h"

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "tcop/utility.h"

#include "cdb/cdbvars.h"
#include "commands/defrem.h"
#include "executor/spi.h"
#include "funcapi.h"

#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#include <sys/stat.h>

#define FILEINFO_ARGS_CNT 5
#define HOUR_TIME 3600000 // milliseconds
#define MINUTE_TIME 60000 // milliseconds
#define FILE_NAPTIME 1    //  milliseconds

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(get_stats_for_database);

static void worker_sigterm(SIGNAL_ARGS);

static Datum *get_databases_oids(int *databases_cnt, MemoryContext ctx);
static int update_segment_file_map_table();
Datum get_stats_for_database(PG_FUNCTION_ARGS);
static void get_stats_for_databases(Datum *databases_oids, int databases_cnt);
static bool plugin_created();
static void collect_stats(Datum main_arg);
void _PG_init(void);
Datum worker_launch(PG_FUNCTION_ARGS);
void _PG_fini(void);

/* flags set by signal handlers */

/* GUC variables */
static int worker_restart_naptime = 0;  /* set up in _PG_init() function */
static int worker_database_naptime = 0; /* set up in _PG_init() function */
static int worker_file_naptime = 0;     /* set up in _PG_init() function */
static bool extension_enabled = false;  /* set up in _PG_init() function */

static void worker_sigterm(SIGNAL_ARGS) {
    int save_errno = errno;
    if (MyProc) {
        SetLatch(&MyProc->procLatch);
    }
    errno = save_errno;
}

static Datum *get_databases_oids(int *databases_cnt, MemoryContext ctx) {
    int retcode = 0;
    char *sql = "SELECT datname, oid FROM pg_database WHERE datname NOT IN ('template0', 'template1', 'diskquota', "
                "'gpperfmon')";
    char *error = NULL;

    Datum *databases_oids = NULL;
    *databases_cnt = 0;

    /* get timestamp and start transaction */
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();

    /* connect to spi */
    retcode = SPI_connect();
    if (retcode < 0) { /* error */
        error = "get_databases_oids: SPI_connect failed";
        goto finish_transaction;
    }
    PushActiveSnapshot(GetTransactionSnapshot());
    pgstat_report_activity(STATE_RUNNING, sql);

    /* execute sql query to get table */
    retcode = SPI_execute(sql, true, 0);

    /* check errors if they're occured during execution */
    if (retcode != SPI_OK_SELECT || SPI_processed < 0) { /* error */
        error = "get_databases_oids: SPI_execute failed (select datname, oid)";
        goto finish_spi;
    }

    /* current store  */
    Datum *tuple_values = palloc0(SPI_tuptable->tupdesc->natts * sizeof(*tuple_values));
    bool *tuple_nullable = palloc0(SPI_tuptable->tupdesc->natts * sizeof(*tuple_nullable));

    /* prepare for coping datum variables */
    bool typByVal;
    int16 typLen;
    char typAlign;

    /* allocate memory for result */
    *databases_cnt = SPI_processed;
    MemoryContext old_context = MemoryContextSwitchTo(ctx);
    databases_oids = palloc0(SPI_tuptable->tupdesc->natts * (*databases_cnt) * sizeof(*databases_oids));
    MemoryContextSwitchTo(old_context);

    for (int i = 0; i < SPI_processed; ++i) {
        /* fetch tuple from tuptable */
        HeapTuple current_tuple = SPI_tuptable->vals[i];
        heap_deform_tuple(current_tuple, SPI_tuptable->tupdesc, tuple_values, tuple_nullable);
        /* store tuple in Datum* */
        old_context = MemoryContextSwitchTo(ctx);
        /* copy datum */
        get_typlenbyvalalign(NAMEOID, &typLen, &typByVal, &typAlign);
        databases_oids[2 * i] = datumCopy(tuple_values[0], typByVal, typLen);
        /* copy datum */
        get_typlenbyvalalign(INT8OID, &typLen, &typByVal, &typAlign);
        databases_oids[2 * i + 1] = datumCopy(tuple_values[1], typByVal, typLen);
        /* sitch back */
        MemoryContextSwitchTo(old_context);
    }

    pfree(tuple_values);
    pfree(tuple_nullable);

finish_spi:
    SPI_finish();
finish_transaction:
    PopActiveSnapshot();
    CommitTransactionCommand();
    pgstat_report_stat(false);
    pgstat_report_activity(STATE_IDLE, NULL);

    if (error != NULL) {
        ereport(ERROR, (errmsg("%s: %m", error)));
    }

    return databases_oids;
}

static int update_segment_file_map_table() {
    int retcode = 0;
    char *sql_truncate = "TRUNCATE TABLE mdb_toolkit.segment_file_map";
    char *sql_insert = "INSERT INTO mdb_toolkit.segment_file_map SELECT gp_segment_id, oid, relfilenode FROM "
                       "gp_dist_random('pg_class')";
    char *error = NULL;

    /* get timestamp and start transaction */
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();

    /* connect to SPI */
    retcode = SPI_connect();
    if (retcode < 0) { /* error */
        error = "update_segment_file_map_table: SPI_connect failed";
        goto finish_transaction;
    }
    PushActiveSnapshot(GetTransactionSnapshot());

    /* update report activity */
    pgstat_report_activity(STATE_RUNNING, sql_truncate);
    /* truncate table */
    retcode = SPI_execute(sql_truncate, false, 0);
    if (retcode != SPI_OK_UTILITY) {
        error = "update_segment_file_map_table: failed to truncate table";
        goto finish_spi;
    }

    /* update report activity */
    pgstat_report_activity(STATE_RUNNING, sql_insert);
    /* insert new rows */
    retcode = SPI_execute(sql_insert, false, 0);
    if (retcode != SPI_OK_INSERT) {
        error = "update_segment_file_map_table: failed to insert new rows into table";
        goto finish_spi;
    }

finish_spi:
    SPI_finish();
finish_transaction:
    PopActiveSnapshot();
    CommitTransactionCommand();
    pgstat_report_stat(false);
    pgstat_report_activity(STATE_IDLE, NULL);

    if (error != NULL) {
        ereport(ERROR, (errmsg("%s: %m", error)));
    }

    return retcode;
}

static bool is_number(char symbol) { return '0' <= symbol && symbol <= '9'; }

/* fill_relfilenode(char *name) - finds first group of nubers in {name}
 * and returns it numeric value
 */
static unsigned int fill_relfilenode(char *name) {
    unsigned int result = 0, pos = 0;
    while (pos < strlen(name) && !is_number(name[pos])) {
        ++pos;
    }
    while (pos < strlen(name) && is_number(name[pos])) {
        result = (result * 10 + (name[pos] - '0'));
        ++pos;
    }
    return result;
}

Datum get_stats_for_database(PG_FUNCTION_ARGS) {
    int retcode;
    int segment_id = GpIdentity.segindex;
    int dboid = PG_GETARG_INT32(0);

    char cwd[PATH_MAX];
    char *data_dir = NULL;
    char *error = NULL;
    char *file_path = NULL;

    getcwd(cwd, sizeof(cwd));
    data_dir = psprintf("%s/base/%d", cwd, dboid);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
    /* Check to see if caller supports us returning a tuplestore */
    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo)) {
        error = "get_stats_for_databa: set-valued function called in context that cannot accept a set";
        goto finish_data;
    }
    if (!(rsinfo->allowedModes & SFRM_Materialize)) {
        error = "get_stats_for_database: materialize mode required, but it is not allowed in this context";
        goto finish_data;
    }

    /* Switch to query context */
    MemoryContext oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
    /* Make the output TupleDesc */
    TupleDesc tupdesc;
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
        MemoryContextSwitchTo(oldcontext);
        error = "get_stats_for_database: incorrect return type in fcinfo (must be a row type)";
        goto finish_data;
    }
    tupdesc = BlessTupleDesc(tupdesc);

    /* Checks if random access is allowed */
    bool randomAccess = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;
    /* Starts the tuplestore */
    Tuplestorestate *tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
    /* Set the output */
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    Datum outputValues[FILEINFO_ARGS_CNT];
    bool outputNulls[FILEINFO_ARGS_CNT];
    MemSet(outputNulls, 0, sizeof(outputNulls));

    /* Returns to the old context */
    MemoryContextSwitchTo(oldcontext);

    DIR *current_dir = AllocateDir(data_dir);
    /* if {current_dir} did not opened => return */
    if (!current_dir) {
        error = "get_stats_for_database: failed to allocate current directory";
        goto finish_data;
    }

    struct dirent *file;
    /* start itterating in {current_dir} */
    while ((file = ReadDir(current_dir, data_dir)) != NULL) {
        char *filename = file->d_name;
        if (strcmp(filename, ".") == 0 ||
            strcmp(filename, "..") == 0) { /* if filename is special as "." or ".." => continue */
            continue;
        }

        file_path = psprintf("%s/%s", data_dir, filename);
        struct stat stb;
        if (lstat(file_path, &stb) < 0) { /* do lstat if returned error => continue */
            ereport(WARNING,
                    (errmsg("get_stats_for_database: lstat failed with %s file (unexpected behavior)", file_path)));
            pfree(file_path);
            continue;
        }
        pfree(file_path);

        if (S_ISREG(stb.st_mode)) {
            /* If file is regular we should count
             * its size and put values into tupstore
             *
             * insert tuple:
             * (segment_is, relfilenode, file_path, stb.st_size, stb.st_mtime)
             */

            outputValues[0] = Int32GetDatum(segment_id);
            outputValues[1] = ObjectIdGetDatum(fill_relfilenode(filename));
            outputValues[2] = CStringGetTextDatum(file_path);
            outputValues[3] = Int64GetDatum(stb.st_size);
            outputValues[4] = Int64GetDatum(stb.st_mtime);

            /* Builds the output tuple (row)
             * and put it in the tuplestore
             */
            tuplestore_putvalues(tupstore, tupdesc, outputValues, outputNulls);

            /* sleep between file proccessing */
            retcode =
                WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, worker_file_naptime);
            ResetLatch(&MyProc->procLatch);

            /* emergency bailout if postmaster has died */
            if (retcode & WL_POSTMASTER_DEATH) {
                proc_exit(1);
            }
        }
    }

    FreeDir(current_dir);
finish_data:
    pfree(data_dir);
    if (error != NULL) {
        ereport(ERROR, (errmsg("%s: %m", error)));
    }

    return (Datum)0;
}

static void get_stats_for_databases(Datum *databases_oids, int databases_cnt) {
    int retcode = 0;
    char *sql = NULL;
    char *error = NULL;

    for (int i = 0; i < databases_cnt; ++i) {
        /* get timestamp and start transaction */
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();

        /* connect to SPI */
        retcode = SPI_connect();
        if (retcode < 0) { /* error */
            error = "get_stats_for_databases: SPI_connect failed";
            goto finish_transaction;
        }
        PushActiveSnapshot(GetTransactionSnapshot());

        char *dbname = NameStr(*DatumGetName(databases_oids[2 * i]));
        int dboid = DatumGetInt32(databases_oids[2 * i + 1]);

        sql = psprintf("SELECT 1 FROM pg_database WHERE oid = %d AND datname = '%s'", dboid, dbname);
        pgstat_report_activity(STATE_RUNNING, sql);

        retcode = SPI_execute(sql, true, 0);
        if (retcode != SPI_OK_SELECT || SPI_processed < 0) { /* error */
            error = "get_stats_for_databases: SPI_execute failed (failed to check if database exists)";
            goto finish_spi;
        }

        if (SPI_processed == 0) {
            continue;
        }

        pfree(sql);
        sql = psprintf("INSERT INTO mdb_toolkit.segment_file_sizes (segment, relfilenode, filepath, size, mtime) "
                       "SELECT * FROM get_stats_for_database(%d)",
                       dboid);
        pgstat_report_activity(STATE_RUNNING, sql);
        retcode = SPI_execute(sql, false, 0);
        if (retcode != SPI_OK_INSERT) {
            error = "get_stats_for_databases: SPI_execute failed (insert into segment_file_sizes)";
            goto finish_spi;
        }

        SPI_finish();
        PopActiveSnapshot();
        CommitTransactionCommand();
        pgstat_report_stat(false);
        pgstat_report_activity(STATE_IDLE, NULL);

        retcode = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                            (worker_database_naptime / databases_cnt));
        ResetLatch(&MyProc->procLatch);

        /* emergency bailout if postmaster has died */
        if (retcode & WL_POSTMASTER_DEATH) {
            proc_exit(1);
        }
    }
    return;
finish_spi:
    SPI_finish();
finish_transaction:
    PopActiveSnapshot();
    CommitTransactionCommand();
    pgstat_report_stat(false);
    pgstat_report_activity(STATE_IDLE, NULL);

    if (sql != NULL) {
        pfree(sql);
    }

    if (error != NULL) {
        ereport(ERROR, (errmsg("%s: %m", error)));
    }
}

static bool plugin_created() {
    int retcode = 0;
    char *sql = "SELECT * FROM pg_extension WHERE extname = 'gp_relsizes_stats'";
    char *error = NULL;

    /* get timestamp and start transaction */
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    /* connect to SPI */
    retcode = SPI_connect();
    if (retcode < 0) { /* error */
        error = "plugin_created: SPI_connect failed";
        goto finish_transaction;
    }
    PushActiveSnapshot(GetTransactionSnapshot());
    pgstat_report_activity(STATE_RUNNING, sql);

    retcode = SPI_execute(sql, true, 0);
    if (retcode != SPI_OK_SELECT || SPI_processed < 0) { /* error */
        error = "plugin_created: SPI_execute failed (failed to check if plugin created)";
        goto finish_spi;
    }

    retcode = SPI_processed;
finish_spi:
    SPI_finish();
finish_transaction:
    PopActiveSnapshot();
    CommitTransactionCommand();
    pgstat_report_stat(false);
    pgstat_report_activity(STATE_IDLE, NULL);

    if (error != NULL) {
        ereport(ERROR, (errmsg("%s: %m", error)));
    }

    return (retcode > 0);
}

static void put_collected_data_into_history() {
    int retcode = 0;
    char *sql_insert =
        "INSERT INTO mdb_toolkit.segment_file_sizes_history SELECT now(), * FROM mdb_toolkit.segment_file_sizes";
    char *sql_truncate = "TRUNCATE TABLE mdb_toolkit.segment_file_sizes";
    char *error = NULL;

    /* get timestamp and start transaction */
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    /* connect to SPI */
    retcode = SPI_connect();
    if (retcode < 0) { /* error */
        error = "put_collected_data_into_history: SPI_connect failed";
        goto finish_transaction;
    }
    PushActiveSnapshot(GetTransactionSnapshot());

    /* update report activity */
    pgstat_report_activity(STATE_RUNNING, sql_insert);
    retcode = SPI_execute(sql_insert, false, 0);
    if (retcode != SPI_OK_INSERT || SPI_processed < 0) { /* error */
        error = "put_collected_data_into_history: SPI_execute failed (failed to insert data into history table)";
        goto finish_spi;
    }

    /* update report activity */
    pgstat_report_activity(STATE_RUNNING, sql_truncate);
    retcode = SPI_execute(sql_truncate, false, 0);
    if (retcode != SPI_OK_UTILITY || SPI_processed < 0) { /* error */
        error = "put_collected_data_into_history: SPI_execute failed (failed to truncate actual table)";
        goto finish_spi;
    }

finish_spi:
    SPI_finish();
finish_transaction:
    PopActiveSnapshot();
    CommitTransactionCommand();
    pgstat_report_stat(false);
    pgstat_report_activity(STATE_IDLE, NULL);

    if (error != NULL) {
        ereport(ERROR, (errmsg("%s: %m", error)));
    }
}

void collect_stats(Datum main_arg) {
    int retcode = 0;
    int databases_cnt;
    char *extension_enabled_option = NULL;
    Datum *databases_oids;

    /* Establish signal handlers before unblocking signals. */
    pqsignal(SIGTERM, worker_sigterm);

    /* We're now ready to receive signals */
    BackgroundWorkerUnblockSignals();

    /* Connect to our database */
    BackgroundWorkerInitializeConnection("postgres", NULL);

    for (;;) {
        extension_enabled_option = GetConfigOptionByName("gp_relsizes_stats.enabled", NULL);

        /* check if plugin created and extension enabled => start working
         * else napping until it will be started
         */
        if (strcmp(extension_enabled_option, "on") != 0 || !plugin_created()) {
            goto naptime;
        }

        /* get list of available databases */
        databases_oids = get_databases_oids(&databases_cnt, CurrentMemoryContext);

        /* update table with mappings (they could be new) */
        PG_TRY();
        { retcode = update_segment_file_map_table(); }
        PG_END_TRY();

        /* get stats for all available databases */
        get_stats_for_databases(databases_oids, databases_cnt);

        /* free databases_oids array */
        pfree(databases_oids);

        /* put all actual data into _history table with timestamps
         * clear actual table after all
         */
        put_collected_data_into_history();
    naptime:
        retcode =
            WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, worker_restart_naptime);
        ResetLatch(&MyProc->procLatch);

        /* emergency bailout if postmaster has died */
        if (retcode & WL_POSTMASTER_DEATH) {
            proc_exit(1);
        }
    }
}

void _PG_init(void) {
    /* define GUC extension enable flag */
    DefineCustomBoolVariable("gp_relsizes_stats.enabled", "Enable extension flag", NULL, &extension_enabled, false,
                             PGC_SIGHUP, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
    /* define GUC naptime variables */
    DefineCustomIntVariable("gp_relsizes_stats.restart_naptime", "Duration between every collect-phases (in sec).",
                            NULL, &worker_restart_naptime,
                            6 * HOUR_TIME, /* set naptime between check-phase (in seconds) */
                            1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("gp_relsizes_stats.database_naptime", "Duration between collect-phase for db (in sec).",
                            NULL, &worker_database_naptime,
                            30 * MINUTE_TIME, /* set naptime between collecting stats of databases (in seconds) */
                            1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("gp_relsizes_stats.file_naptime", "Duration between each collect-phase for files (in sec).",
                            NULL, &worker_file_naptime, FILE_NAPTIME, /* set naptime between check-phase (in seconds) */
                            1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);

    if (!process_shared_preload_libraries_in_progress) {
        return;
    }

    /* allocate shared memory, start background workers, etc */
    BackgroundWorker worker;
    /* set up common data for our worker */
    memset(&worker, 0, sizeof(worker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    worker.bgw_main = collect_stats;
    worker.bgw_notify_pid = 0;
    worker.bgw_start_rule = NULL;

    /* Fill in worker-specific data, and do the actual registrations. */
    snprintf(worker.bgw_name, BGW_MAXLEN, "gp_relsizes_stats_worker");
    worker.bgw_main_arg = Int32GetDatum(0);
    RegisterBackgroundWorker(&worker);
}

void _PG_fini(void) {
    // nothing to do here for this template
}
