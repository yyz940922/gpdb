/*-------------------------------------------------------------------------
 *
 * ginscan.c
 *	  routines to manage scans inverted index relations
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginscan.c,v 1.21 2009/01/10 21:08:36 tgl Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin.h"
#include "access/relscan.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"


Datum
ginbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	int			keysz = PG_GETARG_INT32(1);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(2);
	IndexScanDesc scan;

	scan = RelationGetIndexScan(rel, keysz, scankey);

	PG_RETURN_POINTER(scan);
}

static void
fillScanKey(GinState *ginstate, GinScanKey key, OffsetNumber attnum, Datum query,
			Datum *entryValues, bool *partial_matches, uint32 nEntryValues, 
			StrategyNumber strategy)
{
	uint32		i,
				j;

	key->nentries = nEntryValues;
	key->entryRes = (bool *) palloc0(sizeof(bool) * nEntryValues);
	key->scanEntry = (GinScanEntry) palloc(sizeof(GinScanEntryData) * nEntryValues);
	key->strategy = strategy;
	key->attnum = attnum;
	key->query = query;
	key->firstCall = TRUE;
	ItemPointerSet(&(key->curItem), InvalidBlockNumber, InvalidOffsetNumber);

	for (i = 0; i < nEntryValues; i++)
	{
		key->scanEntry[i].pval = key->entryRes + i;
		key->scanEntry[i].entry = entryValues[i];
		key->scanEntry[i].attnum = attnum;
		ItemPointerSet(&(key->scanEntry[i].curItem), InvalidBlockNumber, InvalidOffsetNumber);
		key->scanEntry[i].offset = InvalidOffsetNumber;
		key->scanEntry[i].buffer = InvalidBuffer;
		key->scanEntry[i].partialMatch = NULL;
		key->scanEntry[i].partialMatchIterator = NULL;
		key->scanEntry[i].partialMatchResult = NULL;
		key->scanEntry[i].strategy = strategy;
		key->scanEntry[i].list = NULL;
		key->scanEntry[i].nlist = 0;
		key->scanEntry[i].isPartialMatch = ( ginstate->canPartialMatch[attnum - 1] && partial_matches ) 
												? partial_matches[i] : false;

		/* link to the equals entry in current scan key */
		key->scanEntry[i].master = NULL;
		for (j = 0; j < i; j++)
			if (compareEntries(ginstate, attnum, entryValues[i], entryValues[j]) == 0)
			{
				key->scanEntry[i].master = key->scanEntry + j;
				break;
			}
	}
}

#ifdef NOT_USED

static void
resetScanKeys(GinScanKey keys, uint32 nkeys)
{
	uint32		i,
				j;

	if (keys == NULL)
		return;

	for (i = 0; i < nkeys; i++)
	{
		GinScanKey	key = keys + i;

		key->firstCall = TRUE;
		ItemPointerSet(&(key->curItem), InvalidBlockNumber, InvalidOffsetNumber);

		for (j = 0; j < key->nentries; j++)
		{
			if (key->scanEntry[j].buffer != InvalidBuffer)
				ReleaseBuffer(key->scanEntry[i].buffer);

			ItemPointerSet(&(key->scanEntry[j].curItem), InvalidBlockNumber, InvalidOffsetNumber);
			key->scanEntry[j].offset = InvalidOffsetNumber;
			key->scanEntry[j].buffer = InvalidBuffer;
			key->scanEntry[j].list = NULL;
			key->scanEntry[j].nlist = 0;
			key->scanEntry[j].partialMatch = NULL;
			key->scanEntry[j].partialMatchIterator = NULL;
			key->scanEntry[j].partialMatchResult = NULL;
		}
	}
}
#endif

static void
freeScanKeys(GinScanKey keys, uint32 nkeys)
{
	uint32		i,
				j;

	if (keys == NULL)
		return;

	for (i = 0; i < nkeys; i++)
	{
		GinScanKey	key = keys + i;

		for (j = 0; j < key->nentries; j++)
		{
			if (key->scanEntry[j].buffer != InvalidBuffer)
				ReleaseBuffer(key->scanEntry[j].buffer);
			if (key->scanEntry[j].list)
				pfree(key->scanEntry[j].list);
			if (key->scanEntry[j].partialMatchIterator)
				tbm_end_iterate(key->scanEntry[j].partialMatchIterator);
			if (key->scanEntry[j].partialMatch)
				tbm_free(key->scanEntry[j].partialMatch);
		}

		pfree(key->entryRes);
		pfree(key->scanEntry);
	}

	pfree(keys);
}

void
newScanKey(IndexScanDesc scan)
{
	ScanKey		scankey = scan->keyData;
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	int			i;
	uint32		nkeys = 0;

	so->keys = (GinScanKey) palloc(scan->numberOfKeys * sizeof(GinScanKeyData));

	if (scan->numberOfKeys < 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("GIN indexes do not support whole-index scans")));

	so->isVoidRes = false;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		Datum	   *entryValues;
		int32		nEntryValues = 0;
		bool	   *partial_matches = NULL;

		/* XXX can't we treat nulls by just setting isVoidRes? */
		/* This would amount to assuming that all GIN operators are strict */
		if (scankey[i].sk_flags & SK_ISNULL)
			elog(ERROR, "GIN doesn't support NULL as scan key");

		entryValues = (Datum *) DatumGetPointer(FunctionCall4(
												&so->ginstate.extractQueryFn[scankey[i].sk_attno - 1],
												scankey[i].sk_argument,
												PointerGetDatum(&nEntryValues),
												UInt16GetDatum(scankey[i].sk_strategy),
												PointerGetDatum(&partial_matches)));
		if (nEntryValues < 0)
		{
			/*
			 * extractQueryFn signals that nothing will be found, so we can
			 * just set isVoidRes flag...
			 */
			so->isVoidRes = true;
			break;
		}

		/*
		 * extractQueryFn signals that everything matches
		 */
		if (entryValues == NULL || nEntryValues == 0)
			/* full scan... */
			continue;

		fillScanKey(&so->ginstate, &(so->keys[nkeys]), scankey[i].sk_attno, scankey[i].sk_argument,
					entryValues, partial_matches, nEntryValues, scankey[i].sk_strategy);
		nkeys++;
	}

	so->nkeys = nkeys;

	if (so->nkeys == 0 && !so->isVoidRes)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("GIN index does not support search with void query")));

	pgstat_count_index_scan(scan->indexRelation);
}

Datum
ginrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(1);
	GinScanOpaque so;

	so = (GinScanOpaque) scan->opaque;

	if (so == NULL)
	{
		/* if called from ginbeginscan */
		so = (GinScanOpaque) palloc(sizeof(GinScanOpaqueData));
		so->tempCtx = AllocSetContextCreate(CurrentMemoryContext,
											"Gin scan temporary context",
											ALLOCSET_DEFAULT_MINSIZE,
											ALLOCSET_DEFAULT_INITSIZE,
											ALLOCSET_DEFAULT_MAXSIZE);
		initGinState(&so->ginstate, scan->indexRelation);
		scan->opaque = so;
	}
	else
	{
		freeScanKeys(so->keys, so->nkeys);
	}

	so->keys = NULL;

	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	}

	PG_RETURN_VOID();
}


Datum
ginendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	if (so != NULL)
	{
		freeScanKeys(so->keys, so->nkeys);

		MemoryContextDelete(so->tempCtx);

		pfree(so);
	}

	PG_RETURN_VOID();
}

Datum
ginmarkpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "GIN does not support mark/restore");
	PG_RETURN_VOID();
}

Datum
ginrestrpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "GIN does not support mark/restore");
	PG_RETURN_VOID();
}
