//////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
//
// This file is part of Ravel.
// Written by Kate Isaacs, kisaacs@acm.org, All rights reserved.
// LLNL-CODE-663885
//
// For details, see https://github.com/scalability-llnl/ravel
// Please also see the LICENSE file for our notice and the LGPL.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License (as published by
// the Free Software Foundation) version 2.1 dated February 1999.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms and
// conditions of the GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//////////////////////////////////////////////////////////////////////////////
#include "otfimporter.h"
#include <QString>
#include <QElapsedTimer>
#include <iostream>
#include <cmath>
#include "general_util.h"
#include "task.h"
#include "rawtrace.h"
#include "commrecord.h"
#include "eventrecord.h"
#include "collectiverecord.h"
#include "function.h"
#include "counter.h"
#include "counterrecord.h"
#include "taskgroup.h"
#include "otfcollective.h"
#include "otf.h"

OTFImporter::OTFImporter()
    : ticks_per_second(0),
      time_conversion_factor(0),
      num_processes(0),
      second_magnitude(1),
      entercount(0),
      exitcount(0),
      sendcount(0),
      recvcount(0),
      enforceMessageSize(false),
      fileManager(NULL),
      otfReader(NULL),
      handlerArray(NULL),
      unmatched_recvs(new QVector<QLinkedList<CommRecord *> *>()),
      unmatched_sends(new QVector<QLinkedList<CommRecord *> *>()),
      rawtrace(NULL),
      tasks(NULL),
      functionGroups(NULL),
      functions(NULL),
      taskgroups(NULL),
      collective_definitions(NULL),
      counters(NULL),
      collectives(NULL),
      collectiveMap(NULL)
{

}

OTFImporter::~OTFImporter()
{

    for (QVector<QLinkedList<CommRecord *> *>::Iterator eitr
         = unmatched_recvs->begin(); eitr != unmatched_recvs->end(); ++eitr)
    {
        for (QLinkedList<CommRecord *>::Iterator itr = (*eitr)->begin();
             itr != (*eitr)->end(); ++itr)
        {
            delete *itr;
            *itr = NULL;
        }
        delete *eitr;
        *eitr = NULL;
    }
    delete unmatched_recvs;

    for (QVector<QLinkedList<CommRecord *> *>::Iterator eitr
         = unmatched_sends->begin();
         eitr != unmatched_sends->end(); ++eitr)
    {
        // Don't delete, used elsewhere
        /*for (QLinkedList<CommRecord *>::Iterator itr = (*eitr)->begin();
         itr != (*eitr)->end(); ++itr)
        {
            delete *itr;
            *itr = NULL;
        }*/
        delete *eitr;
        *eitr = NULL;
    }
    delete unmatched_sends;
}

RawTrace * OTFImporter::importOTF(const char* otf_file, bool _enforceMessageSize)
{
    enforceMessageSize = _enforceMessageSize;
    entercount = 0;
    exitcount = 0;
    sendcount = 0;
    recvcount = 0;

    QElapsedTimer traceTimer;
    qint64 traceElapsed;

    traceTimer.start();

    fileManager = OTF_FileManager_open(1);
    otfReader = OTF_Reader_open(otf_file, fileManager);
    handlerArray = OTF_HandlerArray_open();

    setHandlers();

    tasks = new QMap<int, Task *>();
    functionGroups = new QMap<int, QString>();
    functions = new QMap<int, Function *>();
    taskgroups = new QMap<int, TaskGroup *>();
    collective_definitions = new QMap<int, OTFCollective *>();
    collectives = new QMap<unsigned long long, CollectiveRecord *>();
    counters = new QMap<unsigned int, Counter *>();

    std::cout << "Reading definitions" << std::endl;
    OTF_Reader_readDefinitions(otfReader, handlerArray);

    rawtrace = new RawTrace(num_processes);
    rawtrace->tasks = tasks;
    rawtrace->second_magnitude = second_magnitude;
    rawtrace->functions = functions;
    rawtrace->functionGroups = functionGroups;
    rawtrace->taskgroups = taskgroups;
    rawtrace->collective_definitions = collective_definitions;
    rawtrace->collectives = collectives;
    rawtrace->counters = counters;
    rawtrace->events = new QVector<QVector<EventRecord *> *>(num_processes);
    rawtrace->messages = new QVector<QVector<CommRecord *> *>(num_processes);
    rawtrace->messages_r = new QVector<QVector<CommRecord *> *>(num_processes);
    rawtrace->counter_records = new QVector<QVector<CounterRecord *> *>(num_processes);
    rawtrace->collectiveBits = new QVector<QVector<RawTrace::CollectiveBit *> *>(num_processes);


    delete unmatched_recvs;
    unmatched_recvs = new QVector<QLinkedList<CommRecord *> *>(num_processes);
    delete unmatched_sends;
    unmatched_sends = new QVector<QLinkedList<CommRecord *> *>(num_processes);
    delete collectiveMap;
    collectiveMap = new QVector<QMap<unsigned long long, CollectiveRecord *> *>(num_processes);
    for (int i = 0; i < num_processes; i++) {
        (*unmatched_recvs)[i] = new QLinkedList<CommRecord *>();
        (*unmatched_sends)[i] = new QLinkedList<CommRecord *>();
        (*collectiveMap)[i] = new QMap<unsigned long long, CollectiveRecord *>();
        (*(rawtrace->events))[i] = new QVector<EventRecord *>();
        (*(rawtrace->messages))[i] = new QVector<CommRecord *>();
        (*(rawtrace->messages_r))[i] = new QVector<CommRecord *>();
        (*(rawtrace->counter_records))[i] = new QVector<CounterRecord *>();
        (*(rawtrace->collectiveBits))[i] = new QVector<RawTrace::CollectiveBit *>();
    }

    std::cout << "Reading events" << std::endl;
    OTF_Reader_readEvents(otfReader, handlerArray);

    rawtrace->collectiveMap = collectiveMap;

    OTF_HandlerArray_close(handlerArray);
    OTF_Reader_close(otfReader);
    OTF_FileManager_close(fileManager);
    std::cout << "Finish reading" << std::endl;

    int unmatched_recv_count = 0;
    for (QVector<QLinkedList<CommRecord *> *>::Iterator eitr
         = unmatched_recvs->begin();
         eitr != unmatched_recvs->end(); ++eitr)
    {
        for (QLinkedList<CommRecord *>::Iterator itr = (*eitr)->begin();
             itr != (*eitr)->end(); ++itr)
        {
            unmatched_recv_count++;
            std::cout << "Unmatched RECV " << (*itr)->sender << "->"
                      << (*itr)->receiver << " (" << (*itr)->send_time << ", "
                      << (*itr)->recv_time << ")" << std::endl;
        }
    }
    int unmatched_send_count = 0;
    for (QVector<QLinkedList<CommRecord *> *>::Iterator eitr
         = unmatched_sends->begin();
         eitr != unmatched_sends->end(); ++eitr)
    {
        for (QLinkedList<CommRecord *>::Iterator itr = (*eitr)->begin();
             itr != (*eitr)->end(); ++itr)
        {
            unmatched_send_count++;
            std::cout << "Unmatched SEND " << (*itr)->sender << "->"
                      << (*itr)->receiver << " (" << (*itr)->send_time << ", "
                      << (*itr)->recv_time << ")" << std::endl;
        }
    }
    std::cout << unmatched_send_count << " unmatched sends and "
              << unmatched_recv_count << " unmatched recvs." << std::endl;


    traceElapsed = traceTimer.nsecsElapsed();
    std::cout << "OTF Reading: ";
    gu_printTime(traceElapsed);
    std::cout << std::endl;

    return rawtrace;
}

void OTFImporter::setHandlers()
{
    // Timer
    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleDefTimerResolution,
                                OTF_DEFTIMERRESOLUTION_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this,
                                        OTF_DEFTIMERRESOLUTION_RECORD);

    // Function Groups
    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleDefFunctionGroup,
                                OTF_DEFFUNCTIONGROUP_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this,
                                        OTF_DEFFUNCTIONGROUP_RECORD);

    // Function Names
    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleDefFunction,
                                OTF_DEFFUNCTION_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this,
                                        OTF_DEFFUNCTION_RECORD);

    // Process Info
    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleDefProcess,
                                OTF_DEFPROCESS_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this,
                                        OTF_DEFPROCESS_RECORD);

    // Counter Names
    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleDefCounter,
                                OTF_DEFCOUNTER_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this,
                                        OTF_DEFCOUNTER_RECORD);

    // Enter & Leave
    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleEnter,
                                OTF_ENTER_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this, OTF_ENTER_RECORD);

    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleLeave,
                                OTF_LEAVE_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this,
                                        OTF_LEAVE_RECORD);

    // Send & Receive
    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleSend,
                                OTF_SEND_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this,
                                        OTF_SEND_RECORD);

    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleRecv,
                                OTF_RECEIVE_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this,
                                        OTF_RECEIVE_RECORD);

    // Counter Value
    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleCounter,
                                OTF_COUNTER_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this,
                                        OTF_COUNTER_RECORD);

    // Collectives

    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleDefProcessGroup,
                                OTF_DEFPROCESSGROUP_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this, OTF_DEFPROCESSGROUP_RECORD);

    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleDefCollectiveOperation,
                                OTF_DEFCOLLOP_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this, OTF_DEFCOLLOP_RECORD);


    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleBeginCollectiveOperation,
                                OTF_BEGINCOLLOP_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this, OTF_BEGINCOLLOP_RECORD);

    /* We just store the start times
    OTF_HandlerArray_setHandler(handlerArray,
                                (OTF_FunctionPointer*) &OTFImporter::handleEndCollectiveOperation,
                                OTF_ENDCOLLOP_RECORD);
    OTF_HandlerArray_setFirstHandlerArg(handlerArray, this, OTF_ENDCOLLOP_RECORD);
    */


}

// Find timescale
uint64_t OTFImporter::convertTime(void* userData, uint64_t time)
{
    return (uint64_t) ((double) time)
            * ((OTFImporter *) userData)->time_conversion_factor;
}

int OTFImporter::handleDefTimerResolution(void* userData, uint32_t stream,
                                          uint64_t ticksPerSecond)
{
    Q_UNUSED(stream);
    ((OTFImporter*) userData)->ticks_per_second = ticksPerSecond;
    ((OTFImporter*) userData)->second_magnitude
            = (int) floor(log10(ticksPerSecond));

    double conversion_factor;
    conversion_factor = pow(10, ((OTFImporter*) userData)->second_magnitude)
            / ((double) ticksPerSecond);

    ((OTFImporter*) userData)->time_conversion_factor = conversion_factor;
    return 0;
}

// Function Names & Groups pretty easy
int OTFImporter::handleDefFunctionGroup(void * userData, uint32_t stream,
                                        uint32_t funcGroup, const char * name)
{
    Q_UNUSED(stream);

    (*(((OTFImporter *) userData)->functionGroups))[funcGroup] = QString(name);
    return 0;
}

int OTFImporter::handleDefFunction(void * userData, uint32_t stream,
                                   uint32_t func, const char* name,
                                   uint32_t funcGroup, uint32_t source)
{
    Q_UNUSED(stream);
    Q_UNUSED(source);

    (*(((OTFImporter*) userData)->functions))[func] = new Function(QString(name),
                                                                   funcGroup);
    return 0;
}

// Here the name seems to correspond to the MPI rank. We might want to do some
// processing to get the actual name of the process or have another map.
// For now we can go with the default numbering I guess.
int OTFImporter::handleDefProcess(void * userData, uint32_t stream,
                                  uint32_t process, const char* name,
                                  uint32_t parent)
{
    Q_UNUSED(stream);
    Q_UNUSED(parent);

    ((OTFImporter *) userData)->tasks->insert(process - 1, new Task(process - 1, QString(name)));
    ((OTFImporter *) userData)->num_processes++;
    return 0;
}

int OTFImporter::handleDefCounter(void * userData, uint32_t stream,
                                  uint32_t counter, const char* name,
                                  uint32_t properties, uint32_t counterGroup,
                                  const char* unit)
{
    Q_UNUSED(stream);
    Q_UNUSED(properties);
    Q_UNUSED(counterGroup);
    (*(((OTFImporter*) userData)->counters))[counter] = new Counter(counter,
                                                                    QString(name),
                                                                    QString(unit));
    return 0;
}

int OTFImporter::handleEnter(void * userData, uint64_t time, uint32_t function,
                             uint32_t process, uint32_t source)
{
    Q_UNUSED(source);
    ((*((((OTFImporter*) userData)->rawtrace)->events))[process - 1])->append(new EventRecord(process - 1,
                                                                                              convertTime(userData,
                                                                                                          time),
                                                                                              function,
                                                                                              true));
    return 0;
}

int OTFImporter::handleLeave(void * userData, uint64_t time, uint32_t function,
                             uint32_t process, uint32_t source)
{
    Q_UNUSED(source);
    ((*((((OTFImporter*) userData)->rawtrace)->events))[process - 1])->append(new EventRecord(process - 1,
                                                                                              convertTime(userData,
                                                                                                          time),
                                                                                              function,
                                                                                              false));
    return 0;
}

// Check if two comm records match
// (one that already is a record, one that is just parts)
bool OTFImporter::compareComms(CommRecord * comm, unsigned int sender,
                               unsigned int receiver, unsigned int tag,
                               unsigned int size)
{
    if ((comm->sender != sender) || (comm->receiver != receiver)
            || (comm->tag != tag) || (comm->size != size))
        return false;
    return true;
}

bool OTFImporter::compareComms(CommRecord * comm, unsigned int sender,
                               unsigned int receiver, unsigned int tag)
{
    if ((comm->sender != sender) || (comm->receiver != receiver)
            || (comm->tag != tag))
        return false;
    return true;
}

// Note the send matching doesn't guarantee any particular order of the
// sends/receives in time. We will need to look into this.
int OTFImporter::handleSend(void * userData, uint64_t time, uint32_t sender,
                            uint32_t receiver, uint32_t group, uint32_t type,
                            uint32_t length, uint32_t source)
{
    Q_UNUSED(source);

    // Every time we find a send, check the unmatched recvs
    // to see if it has a match
    time = convertTime(userData, time);
    CommRecord * cr = NULL;
    QLinkedList<CommRecord *> * unmatched = (*(((OTFImporter *) userData)->unmatched_recvs))[sender - 1];
    bool useSize = ((OTFImporter *) userData)->enforceMessageSize;
    for (QLinkedList<CommRecord *>::Iterator itr = unmatched->begin();
         itr != unmatched->end(); ++itr)
    {
        if (useSize ? OTFImporter::compareComms((*itr), sender, receiver, type, length)
                    : OTFImporter::compareComms((*itr), sender, receiver, type))
        {
            cr = *itr;
            cr->send_time = time;
            ((*((((OTFImporter*) userData)->rawtrace)->messages))[sender - 1])->append((cr));
            break;
        }
    }


    // If we did find a match, remove it from the unmatched.
    // Otherwise, create a new unmatched send record
    if (cr)
    {
        (*(((OTFImporter *) userData)->unmatched_recvs))[sender - 1]->removeOne(cr);
    }
    else
    {
        cr = new CommRecord(sender - 1, time, receiver - 1, 0, length, type, group);
        (*((((OTFImporter*) userData)->rawtrace)->messages))[sender - 1]->append(cr);
        (*(((OTFImporter *) userData)->unmatched_sends))[sender - 1]->append(cr);
    }
    return 0;
}


int OTFImporter::handleRecv(void * userData, uint64_t time, uint32_t receiver,
                            uint32_t sender, uint32_t group, uint32_t type,
                            uint32_t length, uint32_t source)
{
    Q_UNUSED(source);

    // Look for match in unmatched_sends
    time = convertTime(userData, time);
    CommRecord * cr = NULL;
    QLinkedList<CommRecord *> * unmatched = (*(((OTFImporter*) userData)->unmatched_sends))[sender - 1];
    bool useSize = ((OTFImporter *) userData)->enforceMessageSize;
    for (QLinkedList<CommRecord *>::Iterator itr = unmatched->begin();
         itr != unmatched->end(); ++itr)
    {
        if (useSize ? OTFImporter::compareComms((*itr), sender -1, receiver -1, type, length)
                    : OTFImporter::compareComms((*itr), sender -1, receiver -1, type))
        {
            cr = *itr;
            cr->recv_time = time;
            break;
        }
    }

    // If match is found, remove it from unmatched_sends, otherwise create
    // a new unmatched recv record
    if (cr)
    {
        (*(((OTFImporter *) userData)->unmatched_sends))[sender - 1]->removeOne(cr);
    }
    else
    {
        cr = new CommRecord(sender - 1, 0, receiver - 1, time, length, type, group);
        ((*(((OTFImporter*) userData)->unmatched_recvs))[sender - 1])->append(cr);
    }
    (*((((OTFImporter*) userData)->rawtrace)->messages_r))[receiver - 1]->append(cr);

    return 0;
}

int OTFImporter::handleCounter(void * userData, uint64_t time,
                               uint32_t process, uint32_t counter,
                               uint64_t value)
{
    CounterRecord * cr = new CounterRecord(counter, convertTime(userData, time), value);
    (*((((OTFImporter *) userData)->rawtrace)->counter_records))[process - 1]->append(cr);
    return 0;
}

// These are for collectives
int OTFImporter::handleDefProcessGroup(void * userData, uint32_t stream,
                                       uint32_t procGroup, const char * name,
                                       uint32_t numberOfProcs,
                                       const uint32_t * procs)
{
    Q_UNUSED(stream);

    QString qname(name);
    if (qname.contains("MPI_COMM_SELF")) // Probably won't use this
        return 0;

    TaskGroup * t = new TaskGroup(procGroup, qname);
    for (int i = 0; i < numberOfProcs; i++)
    {
        t->tasks->append(procs[i] - 1);
        t->taskorder->insert(procs[i] - 1, i);
    }
    (*(((OTFImporter*) userData)->taskgroups))[procGroup] = t;

    return 0;
}

int OTFImporter::handleDefCollectiveOperation(void * userData, uint32_t stream,
                                              uint32_t collOp, const char * name,
                                              uint32_t type)
{
    Q_UNUSED(stream);

    (*(((OTFImporter *) userData)->collective_definitions))[collOp]
            = new OTFCollective(collOp, type, name);

    return 0;
}

// Deprecated
int OTFImporter::handleCollectiveOperation(void * userData, uint64_t time,
                                           uint32_t process,
                                           uint32_t collective,
                                           uint32_t procGroup,
                                           uint32_t rootProc,
                                           uint32_t sent, uint32_t received,
                                           uint64_t duration,
                                           uint32_t source)
{
    Q_UNUSED(source); // Location in source code
    Q_UNUSED(userData);

    std::cout << "Collective: " << time << ", proc: " << process << ", coll: " << collective;
    std::cout << ", root: " << rootProc << ", group: " << procGroup << ", sent: " << sent;
    std::cout << ", recv: " << received << ", dur: " << duration << std::endl;

    return 0;
}

int OTFImporter::handleBeginCollectiveOperation(void * userData, uint64_t time,
                                                uint32_t process,
                                                uint32_t collective,
                                                uint64_t matchingId,
                                                uint32_t procGroup,
                                                uint32_t rootProc,
                                                uint32_t sent,
                                                uint32_t received,
                                                uint32_t scltoken,
                                                OTF_KeyValueList * list)
{
    Q_UNUSED(list);
    Q_UNUSED(scltoken);
    Q_UNUSED(sent); // Data volume received
    Q_UNUSED(received); // Data volume sent

    // Convert rootProc to 0..p-1 space if it truly is a root and not unrooted value
    if (rootProc > 0)
        rootProc--;

    // Create collective record if it doesn't yet exist
    if (!(*(((OTFImporter *) userData)->collectives)).contains(matchingId))
        (*(((OTFImporter *) userData)->collectives))[matchingId]
            = new CollectiveRecord(matchingId, rootProc, collective, procGroup);

    // Get the matching collective record
    CollectiveRecord * cr = (*(((OTFImporter *) userData)->collectives))[matchingId];

    // Map process/time to the collective record
    time = convertTime(userData, time);
    (*(*(((OTFImporter *) userData)->collectiveMap))[process - 1])[time] = cr;
    ((OTFImporter *) userData)->rawtrace->collectiveBits->at(process - 1)->append(new RawTrace::CollectiveBit(time, cr));

    return 0;
}

int OTFImporter::handleEndCollectiveOperation(void * userData, uint64_t time,
                                              uint32_t process,
                                              uint64_t matchingId,
                                              OTF_KeyValueList * list)
{
    Q_UNUSED(userData);
    Q_UNUSED(list);

    return 0;
}
