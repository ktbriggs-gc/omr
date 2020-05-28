/*******************************************************************************
 * Copyright (c) 2018, 2018 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#ifndef EVACUATORHISTORY_HPP_
#define EVACUATORHISTORY_HPP_

#include "EvacuatorBase.hpp"

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)

class MM_EvacuatorHistory
{
/*
 * Data members
 */
public:
	static const uintptr_t maxEpoch = MM_EvacuatorBase::epochs_per_cycle;

	/* (rarely) if the epoch counter overflows epoch history record capacity the last history record is reused */
	typedef struct Epoch {
		uintptr_t gc;							/* sequential gc cycle number */
		uintptr_t epoch;						/* sequential epoch number */
		uint64_t duration;						/* epoch duration in microseconds */
		uintptr_t survivorCopied;				/* cumulative survivor copied byte count */
		uintptr_t tenureCopied;					/* cumulative tenure copied byte count */
		uintptr_t scanned;						/* cumulative scanned byte count */
		uintptr_t cleared;						/* instantaneous sample of empty evacuator stack count */
		uintptr_t stalled;						/* instantaneous sample of stalled evacuator count */
		uintptr_t survivorAllocationCeiling;	/* upper limit on TLH allocation size for survivor */
		uintptr_t tenureAllocationCeiling;		/* upper limit on TLH allocation size for tenure */
		uintptr_t minVolumeOfWork;				/* least volume of work per evacuator at end of epoch */
		uintptr_t maxVolumeOfWork;				/* least volume of work per evacuator at end of epoch */
		uintptr_t sumVolumeOfWork;				/* total volume of work per all evacuators at end of epoch */
		uintptr_t volumeHistogram[3];			/* number of evacuators with 0|<threshold|>threshold volume of work */
		uintptr_t volumeQuota;					/* controller work notification threshold at end of epoch */
	} Epoch;

protected:

private:
	uintptr_t _epoch;							/* current epoch (incomplete if not at end of scan cycle) */
	uintptr_t _timestamp;						/* marks start of current (uncommitted) epoch */
	intptr_t _last;								/* index of most recently committed epoch may be <(_epoch-1) */
	Epoch _history[maxEpoch];					/* epochal record spanning one gc cycle */

/*
 * Function members
 */
private:
protected:

public:
	/* epochs are unrecorded until duration is set */
	bool isRecorded(Epoch *epoch) { return (NULL != epoch) && (0 < epoch->tenureAllocationCeiling) && (0 < epoch->survivorAllocationCeiling); }

	/* get a past (committed) or the current (uncommitted) epoch */
	Epoch *getEpoch(uintptr_t index) { return &_history[(index < maxEpoch) ? index : (maxEpoch - 1)]; }

	/* get the most recently committed epoch (or an empty epoch record if none committed yet in which case _last is <0) */
	const Epoch *getEpoch() { return getEpoch((0 < _last) ? (uintptr_t)_last : 0); }

	/* reserve tail of historic record to commit stats at end of current epoch */
	Epoch *nextEpoch(uintptr_t next)
	{
		if (maxEpoch <= next) {
			next = maxEpoch - 1;
		}
		if (_last < (intptr_t)next) {
			_last = (intptr_t)next;
		}
		return &_history[next];
	}

	/* swap a new timestamp for start of current epoch */
	uintptr_t epochStartTime(uintptr_t currentTimestamp)
	{
		uintptr_t timestamp = _timestamp;
		_timestamp = currentTimestamp;

		return timestamp;
	}

	/* return most recently recorded epoch previous to input epoch */
	Epoch *priorEpoch(Epoch *epoch)
	{
		MM_EvacuatorHistory::Epoch *previous = epoch - 1;
		while ((previous > &_history[0]) && !isRecorded(previous)) {
			previous -= 1;
		}
		return (previous > &_history[0]) ? previous : epoch;
	}

	/* clear history for starting a gc cycle */
	void
	reset(uintptr_t gc = 0, uintptr_t timestamp = 0, uintptr_t survivorAllocationCeiling = 0, uintptr_t tenureAllocationCeiling = 0)
	{
		memset(&_history[0], 0, (maxEpoch * sizeof(Epoch)));

		_history[0].survivorAllocationCeiling = survivorAllocationCeiling;
		_history[0].tenureAllocationCeiling = tenureAllocationCeiling;
		_history[0].gc = gc;

		_timestamp = timestamp;
		_epoch = 0;
		_last = -1;
	}

	MM_EvacuatorHistory()
	{
		reset();
	}
};

#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

#endif /* EVACUATORHISTORY_HPP_ */
