/*-------------------------------------------------------------------------
 *
 * worker_transaction.c
 *
 * Routines for performing transactions across all workers.
 *
 * Copyright (c) 2013-2016, Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "distributed/connection_cache.h"
#include "distributed/multi_transaction.h"
#include "distributed/worker_manager.h"
#include "distributed/worker_transaction.h"
#include "utils/memutils.h"


/* Local functions forward declarations */
static List * OpenWorkerTransactions(void);
static void CompleteWorkerTransactions(XactEvent event, void *arg);
static void CloseWorkerConnections(void);


/* Global worker connection list */
static List *workerConnectionList = NIL;
static bool isXactCallbackRegistered = false;


/*
 * SendCommandToWorkersInOrder sends a command to all workers in order.
 * Commands are committed on the workers when the local transaction
 * commits.
 */
void
SendCommandToWorkersInOrder(char *command)
{
	ListCell *connectionCell = NULL;

	List *connectionList = OpenWorkerTransactions();

	foreach(connectionCell, connectionList)
	{
		TransactionConnection *transactionConnection =
			(TransactionConnection *) lfirst(connectionCell);

		PGconn *connection = transactionConnection->connection;

		PGresult *result = PQexec(connection, command);
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			char *nodeName = ConnectionGetOptionValue(connection, "host");
			char *nodePort = ConnectionGetOptionValue(connection, "port");

			ReportRemoteError(connection, result);

			ereport(ERROR, (errmsg("failed to send metadata change to %s:%s",
								   nodeName, nodePort)));
		}
	}
}


/*
 * SendCommandToWorkersInParallel sends a command to all workers in
 * parallel. Commands are committed on the workers when the local
 * transaction commits.
 */
void
SendCommandToWorkersInParallel(char *command)
{
	ListCell *connectionCell = NULL;

	List *connectionList = OpenWorkerTransactions();

	foreach(connectionCell, connectionList)
	{
		TransactionConnection *transactionConnection =
			(TransactionConnection *) lfirst(connectionCell);

		PGconn *connection = transactionConnection->connection;

		int querySent = PQsendQuery(connection, command);
		if (querySent == 0)
		{
			char *nodeName = ConnectionGetOptionValue(connection, "host");
			char *nodePort = ConnectionGetOptionValue(connection, "port");

			ReportRemoteError(connection, NULL);

			ereport(ERROR, (errmsg("failed to send metadata change to %s:%s",
								   nodeName, nodePort)));
		}
	}

	foreach(connectionCell, connectionList)
	{
		TransactionConnection *transactionConnection =
			(TransactionConnection *) lfirst(connectionCell);

		PGconn *connection = transactionConnection->connection;

		PGresult *result = PQgetResult(connection);
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			char *nodeName = ConnectionGetOptionValue(connection, "host");
			char *nodePort = ConnectionGetOptionValue(connection, "port");

			ReportRemoteError(connection, result);
			PQclear(result);

			ereport(ERROR, (errmsg("failed to apply metadata change on %s:%s",
								   nodeName, nodePort)));
		}

		PQclear(result);

		/* clear NULL result */
		PQgetResult(connection);
	}
}


/*
 * OpenWorkerTransactions opens connections to all workers and sends
 * BEGIN commands. Once opened, the remote transaction are committed
 * or aborted when the local transaction commits or aborts. Multiple
 * invocations of OpenWorkerTransactions will return the same list
 * of connections until the commit/abort.
 */
static List *
OpenWorkerTransactions(void)
{
	ListCell *workerNodeCell = NULL;
	List *connectionList = NIL;
	MemoryContext oldContext = NULL;
	List *workerList = WorkerNodeList();
	int workerCount = list_length(workerList);
	int workerConnectionCount = list_length(workerConnectionList);

	/* a new node is added to the cluster, re-open the connections */
	if (workerCount != workerConnectionCount)
	{
		CloseConnections(workerConnectionList);
		workerConnectionList = NIL;
	}

	/* connections were cached, we need to drop them as well */
	if (workerConnectionList != NIL)
	{
		return workerConnectionList;
	}

	/* TODO: lock worker list */
	oldContext = MemoryContextSwitchTo(TopTransactionContext);

	foreach(workerNodeCell, workerList)
	{
		WorkerNode *workerNode = (WorkerNode *) lfirst(workerNodeCell);
		char *nodeName = workerNode->workerName;
		int nodePort = workerNode->workerPort;
		PGconn *connection = NULL;

		TransactionConnection *transactionConnection = NULL;
		PGresult *result = NULL;

		connection = GetOrEstablishConnection(nodeName, nodePort);
		if (connection == NULL)
		{
			ereport(ERROR, (errmsg("could not open connection to %s:%d",
								   nodeName, nodePort)));
		}

		result = PQexec(connection, "BEGIN");
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			ReportRemoteError(connection, result);
			PQclear(result);

			ereport(ERROR, (errmsg("could not start transaction on %s:%d",
								   nodeName, nodePort)));
		}

		PQclear(result);

		transactionConnection = palloc0(sizeof(TransactionConnection));

		transactionConnection->connectionId = 0;
		transactionConnection->transactionState = TRANSACTION_STATE_OPEN;
		transactionConnection->connection = connection;

		connectionList = lappend(connectionList, transactionConnection);
	}

	MemoryContextSwitchTo(oldContext);

	if (!isXactCallbackRegistered)
	{
		RegisterXactCallback(CompleteWorkerTransactions, NULL);
		isXactCallbackRegistered = true;
	}

	workerConnectionList = connectionList;

	return connectionList;
}


/*
 * CompleteWorkerTransaction commits or aborts pending worker transactions
 * when the local transaction commits or aborts.
 */
static void
CompleteWorkerTransactions(XactEvent event, void *arg)
{
	if (workerConnectionList == NIL)
	{
		/* nothing to do */
		return;
	}
	else if (event == XACT_EVENT_PRE_COMMIT)
	{
		/*
		 * Any failure here will cause local changes to be rolled back,
		 * and remote changes to either roll back (1PC) or, in case of
		 * connection or node failure, leave a prepared transaction
		 * (2PC).
		 */

		if (MultiShardCommitProtocol == COMMIT_PROTOCOL_2PC)
		{
			PrepareRemoteTransactions(workerConnectionList);
		}

		return;
	}
	else if (event == XACT_EVENT_COMMIT)
	{
		/*
		 * A failure here will cause some remote changes to either
		 * roll back (1PC) or, in case of connection or node failure,
		 * leave a prepared transaction (2PC). However, the local
		 * changes have already been committed.
		 */

		CommitRemoteTransactions(workerConnectionList, false);
	}
	else if (event == XACT_EVENT_ABORT)
	{
		/*
		 * A failure here will cause some remote changes to either
		 * roll back (1PC) or, in case of connection or node failure,
		 * leave a prepared transaction (2PC). The local changes have
		 * already been rolled back.
		 */

		AbortRemoteTransactions(workerConnectionList);
	}
	else
	{
		return;
	}

	workerConnectionList = NIL;
}
