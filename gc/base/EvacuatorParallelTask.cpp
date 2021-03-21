/*******************************************************************************
 * Copyright (c) 2015, 2018 IBM Corp. and others
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

#include "omrcfg.h"

#if defined(OMR_GC_MODRON_SCAVENGER)

#include "ModronAssertions.h"

#include "EnvironmentStandard.hpp"
#include "Evacuator.hpp"
#include "EvacuatorController.hpp"

#include "EvacuatorParallelTask.hpp"

void
MM_EvacuatorParallelTask::run(MM_EnvironmentBase *envBase)
{
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);

	/* send bound evacuator off to work ... */
	env->getEvacuator()->workThreadGarbageCollect(env);
}

void
MM_EvacuatorParallelTask::masterSetup(MM_EnvironmentBase *envBase)
{
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);
	if (env->isMasterThread()) {
		/* instantiate or resurrect evacuator instances per dispatched thread count */
		_controller-> bindWorkers(env);
	} else {
		Assert_MM_true(NULL == env->_cycleState);
		env->_cycleState = _cycleState;
	}
}

void
MM_EvacuatorParallelTask::setup(MM_EnvironmentBase *envBase)
{
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);
	/* bind dispatched thread to an evacuator instance */
	_controller-> bindWorker(env);
	if (env->isMasterThread()) {
		Assert_MM_true(_cycleState == env->_cycleState);
	} else {
		Assert_MM_true(NULL == env->_cycleState);
		env->_cycleState = _cycleState;
	}
}

void
MM_EvacuatorParallelTask::cleanup(MM_EnvironmentBase *envBase)
{
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);
	/* release evacuator binding and passivate evacuator instance */
	_controller->unbindWorker(env);
	if (env->isMasterThread()) {
		Assert_MM_true(_cycleState == env->_cycleState);
	} else {
		env->_cycleState = NULL;
	}
}

void
MM_EvacuatorParallelTask::complete(MM_EnvironmentBase *envBase)
{
	MM_ParallelTask::complete(envBase);
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);
	if (env->isMasterThread()) {
		_controller->aggregateEvacuatorMetrics(env);
	}
}

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
void
MM_EvacuatorParallelTask::synchronizeGCThreads(MM_EnvironmentBase *envBase, const char *id)
{
	OMRPORT_ACCESS_FROM_OMRPORT(envBase->getPortLibrary());
	uint64_t startTime = omrtime_hires_clock();

	MM_ParallelTask::synchronizeGCThreads(envBase, id);

	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);
	env->getMetrics()->_threadMetrics[MM_Evacuator::sync_ms] += (omrtime_hires_clock() - startTime);
	env->getMetrics()->_threadMetrics[MM_Evacuator::sync_count] += 1;
}

bool
MM_EvacuatorParallelTask::synchronizeGCThreadsAndReleaseMaster(MM_EnvironmentBase *envBase, const char *id)
{
	OMRPORT_ACCESS_FROM_OMRPORT(envBase->getPortLibrary());
	uint64_t startTime = omrtime_hires_clock();

	bool isMasterThread = false;

	if (MM_ParallelTask::synchronizeGCThreadsAndReleaseMaster(envBase, id)) {
		Debug_MM_true(!isMasterThread || (0 == *(_controller->sampleStalledMap())));
		Debug_MM_true(!isMasterThread || (0 == *(_controller->sampleResumingMap())));
		Debug_MM_true(!isMasterThread || (!_controller->areAnyEvacuatorsStalled()));
		isMasterThread = true;
	}
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);
	env->getMetrics()->_threadMetrics[MM_Evacuator::sync_ms] += (omrtime_hires_clock() - startTime);
	env->getMetrics()->_threadMetrics[MM_Evacuator::sync_count] += 1;

	return isMasterThread;
}

bool
MM_EvacuatorParallelTask::synchronizeGCThreadsAndReleaseSingleThread(MM_EnvironmentBase *envBase, const char *id)
{
	OMRPORT_ACCESS_FROM_OMRPORT(envBase->getPortLibrary());
	uint64_t startTime = omrtime_hires_clock();

	bool isReleasedThread = false;

	if (MM_ParallelTask::synchronizeGCThreadsAndReleaseSingleThread(envBase, id)) {
		Debug_MM_true(!isReleasedThread || (0 == *(_controller->sampleStalledMap())));
		Debug_MM_true(!isReleasedThread || (0 == *(_controller->sampleResumingMap())));
		Debug_MM_true(!isReleasedThread || (!_controller->areAnyEvacuatorsStalled()));
		isReleasedThread = true;
	}
	MM_EnvironmentStandard *env = MM_EnvironmentStandard::getEnvironment(envBase);
	env->getMetrics()->_threadMetrics[MM_Evacuator::sync_ms] += (omrtime_hires_clock() - startTime);
	env->getMetrics()->_threadMetrics[MM_Evacuator::sync_count] += 1;

	return isReleasedThread;
}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
void
MM_EvacuatorParallelTask::releaseSynchronizedGCThreads(MM_EnvironmentBase *envBase)
{
	Debug_MM_true(0 == *(_controller->sampleStalledMap()));
	Debug_MM_true(0 == *(_controller->sampleResumingMap()));
	Debug_MM_true(!_controller->areAnyEvacuatorsStalled());

	MM_ParallelTask::releaseSynchronizedGCThreads(envBase);
}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
#endif /* J9MODRON_TGC_PARALLEL_STATISTICS */

#endif /* defined(OMR_GC_MODRON_SCAVENGER) */
