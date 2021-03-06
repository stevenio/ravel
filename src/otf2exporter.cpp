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
#include "otf2exporter.h"
#include "trace.h"
#include "event.h"
#include "commevent.h"
#include "task.h"
#include "taskgroup.h"
#include "function.h"
#include "rpartition.h"
#include <climits>
#include <cmath>
#include <iostream>

OTF2Exporter::OTF2Exporter(Trace *_t)
    : trace(_t),
      ravel_string(0),
      ravel_version_string(0),
      archive(NULL),
      global_def_writer(NULL),
      inverseStringMap(QMap<QString, int>()),
      attributeMap(new QMap<QString, int>())
{
    flush_callbacks.otf2_post_flush = OTF2Exporter::post_flush;
    flush_callbacks.otf2_pre_flush = OTF2Exporter::pre_flush;
}

OTF2Exporter::~OTF2Exporter()
{
    delete attributeMap;
}

void OTF2Exporter::exportTrace(QString path, QString filename)
{
    // Setup the IDs for partition identification
    for (int i = 0; i < trace->partitions->size(); i++)
    {
        Partition * p = trace->partitions->at(i);
        for (QMap<int, QList<CommEvent *> *>::Iterator elist = p->events->begin();
             elist != p->events->end(); ++elist)
        {
            for (QList<CommEvent *>::Iterator evt = (elist.value())->begin();
                 evt != (elist.value())->end(); ++evt)
            {
                (*evt)->phase = i;
            }
        }

    }

    archive = OTF2_Archive_Open(path.toStdString().c_str(),
                                filename.toStdString().c_str(),
                                OTF2_FILEMODE_WRITE,
                                1024 * 1024, 4 * 1024 * 1024,
                                OTF2_SUBSTRATE_POSIX, OTF2_COMPRESSION_NONE);

    OTF2_Archive_SetFlushCallbacks(archive, &flush_callbacks, NULL);
    OTF2_Archive_SetSerialCollectiveCallbacks(archive);
    exportDefinitions();
    exportEvents();

    OTF2_Archive_Close(archive);
}

void OTF2Exporter::exportEvents()
{
    OTF2_Archive_OpenEvtFiles(archive);

    for (QMap<int, Task *>::Iterator task = trace->tasks->begin();
         task != trace->tasks->end(); ++task)
    {
        exportTaskEvents(task.key());
    }

    OTF2_Archive_CloseEvtFiles(archive);
}

void OTF2Exporter::exportTaskEvents(int taskid)
{
    QVector<Event *> * roots = trace->roots->at(taskid);
    OTF2_EvtWriter * evt_writer = OTF2_Archive_GetEvtWriter(archive,
                                                            taskid);
    for (QVector<Event *>::Iterator root = roots->begin();
         root != roots->end(); ++root)
    {
        (*root)->writeToOTF2(evt_writer, attributeMap);
    }

    OTF2_Archive_CloseEvtWriter(archive, evt_writer);
}

void OTF2Exporter::exportDefinitions()
{
    global_def_writer = OTF2_Archive_GetGlobalDefWriter(archive);

    // Write time properties
    unsigned long long start = ULLONG_MAX, end = 0;
    for (QVector<QVector<Event *> *>::Iterator evts = trace->events->begin();
         evts != trace->events->end(); ++evts)
    {
        if ((*evts)->first()->enter < start)
            start = (*evts)->first()->enter;
        if ((*evts)->last()->exit > end)
            end = (*evts)->last()->enter;
    }
    OTF2_GlobalDefWriter_WriteClockProperties(global_def_writer,
                                              pow(10, trace->units),
                                              start,
                                              end - start + 1);

    // Strings we will need for rest of the definitions
    exportStrings();

    exportAttributes();
    exportFunctions();
    exportTasks();
    exportTaskGroups();

}

// This includes both Ravel information and any ravel metrics.
// We do this as attributes to keep with the event rather than separate.
void OTF2Exporter::exportAttributes()
{
    // Metrics
    int id = 0;
    for (QList<QString>::Iterator metric = trace->metrics->begin();
         metric != trace->metrics->end(); ++metric)
    {
        // One for the normal where we put the unit in the description
        OTF2_GlobalDefWriter_WriteAttribute(global_def_writer,
                                            id,
                                            inverseStringMap.value(*metric),
                                            inverseStringMap.value(trace->metric_units->value(*metric)),
                                            OTF2_TYPE_UINT64);
        attributeMap->insert(*metric, id);
        id++;

        // One for the aggregate
        OTF2_GlobalDefWriter_WriteAttribute(global_def_writer,
                                            id,
                                            inverseStringMap.value(*metric + "_agg"),
                                            inverseStringMap.value(trace->metric_units->value(*metric)),
                                            OTF2_TYPE_UINT64);
        attributeMap->insert(*metric + "_agg", id);
        id++;
    }


    // Phase attribute
    OTF2_GlobalDefWriter_WriteAttribute(global_def_writer,
                                        id,
                                        ravel_version_string + 1,
                                        0,
                                        OTF2_TYPE_UINT64);
    attributeMap->insert("phase", id);
    id++;

    // Step attribute
    OTF2_GlobalDefWriter_WriteAttribute(global_def_writer,
                                        id,
                                        ravel_version_string + 2,
                                        0,
                                        OTF2_TYPE_UINT64);
    attributeMap->insert("step", id);
    id++;

    // Write Ravel information
    OTF2_GlobalDefWriter_WriteAttribute(global_def_writer,
                                        id,
                                        ravel_string, // Ravel string ref
                                        ravel_version_string, // Ravel string version
                                        OTF2_TYPE_NONE);
    id++;


    QList<QString> options = trace->options.getOptionNames();
    for (QList<QString>::Iterator opt = options.begin();
         opt != options.end(); ++opt)
    {
        OTF2_GlobalDefWriter_WriteAttribute(global_def_writer,
                                            id,
                                            inverseStringMap.value(*opt), // option name
                                            inverseStringMap.value(trace->options.getOptionValue(*opt)), // value
                                            OTF2_TYPE_NONE);
        id++;
    }
}

void OTF2Exporter::exportTaskGroups()
{
    // Communicators
    for (QMap<int, TaskGroup *>::Iterator tg = trace->taskgroups->begin();
         tg != trace->taskgroups->end(); ++tg)
    {
        int num_tasks = (tg.value())->tasks->size();
        uint64_t tasks[num_tasks];
        for (int i = 0; i < num_tasks; i++)
            tasks[i] = (tg.value())->tasks->at(i);

        OTF2_GlobalDefWriter_WriteGroup(global_def_writer,
                                        (tg.value())->id /* id */,
                                        inverseStringMap.value((tg.value())->name) /* name */,
                                        OTF2_GROUP_TYPE_COMM_LOCATIONS,
                                        OTF2_PARADIGM_MPI,
                                        OTF2_GROUP_FLAG_NONE,
                                        num_tasks,
                                        tasks);

        OTF2_GlobalDefWriter_WriteComm(global_def_writer,
                                       (tg.value())->id /* id */,
                                       inverseStringMap.value((tg.value())->name) /* name */,
                                       (tg.value())->id /* group */,
                                       OTF2_UNDEFINED_COMM /* parent */ );

    }

    // MPI Paradigm Group to be added
    // Create locations group
    uint64_t comm_locations[trace->tasks->size()];
    int j = 0;
    for (QMap<int, Task *>::Iterator task = trace->tasks->begin();
         task != trace->tasks->end(); ++task)
    {
        comm_locations[j] = (task.value())->id;
        j++;
    }
    // Find a unique ID
    int id = 0;
    while (trace->taskgroups->contains(id))
        id++;

    // Write group
    OTF2_GlobalDefWriter_WriteGroup( global_def_writer,
                                     id /* id */,
                                     0 /* name */,
                                     OTF2_GROUP_TYPE_COMM_LOCATIONS,
                                     OTF2_PARADIGM_MPI,
                                     OTF2_GROUP_FLAG_NONE,
                                     trace->tasks->size(),
                                     comm_locations );

}

void OTF2Exporter::exportTasks()
{
    for (QMap<int, Task *>::Iterator task = trace->tasks->begin();
         task != trace->tasks->end(); ++task)
    {
        OTF2_GlobalDefWriter_WriteLocationGroup(global_def_writer,
                                                (task.value())->id /* id */,
                                                inverseStringMap.value((task.value())->name) /* name */,
                                                OTF2_LOCATION_GROUP_TYPE_PROCESS,
                                                0 /* system tree */ );

        // TODO: Generalize this for non MPI tasks at #events
        OTF2_GlobalDefWriter_WriteLocation(global_def_writer,
                                           (task.value())->id /* id */,
                                           inverseStringMap.value((task.value())->name) /* name */,
                                           OTF2_LOCATION_TYPE_CPU_THREAD,
                                           trace->events->at(task.key())->size() /* # events */,
                                           (task.value())->id /* location group */ );
    }
}

void OTF2Exporter::exportFunctions()
{
    for (QMap<int, Function *>::Iterator fxn = trace->functions->begin();
         fxn != trace->functions->end(); ++fxn)
    {
        OTF2_GlobalDefWriter_WriteRegion( global_def_writer,
                                          fxn.key() /* id */,
                                          inverseStringMap.value((fxn.value())->name) /* region name  */,
                                          0 /* alternative name */,
                                          0 /* description */,
                                          OTF2_REGION_ROLE_UNKNOWN,
                                          (trace->functionGroups->contains((fxn.value())->group)
                                           && trace->functionGroups->value(fxn.value()->group).contains("MPI"))
                                          ? OTF2_PARADIGM_MPI
                                          : OTF2_PARADIGM_UNKNOWN,
                                          OTF2_REGION_FLAG_NONE,
                                          0 /* source file */,
                                          0 /* begin lno */,
                                          0 /* end lno */ );
    }
}

void OTF2Exporter::exportStrings()
{
    int counter = 0;
    counter = addString("", counter);
    counter = addString("Ravel", counter);
    ravel_string = counter;
    counter = addString("0.9.0", counter);
    ravel_version_string = counter;
    counter = addString("phase", counter);
    counter = addString("step", counter);

    for (QMap<int, Task *>::Iterator task = trace->tasks->begin();
         task != trace->tasks->end(); ++task)
    {
        counter = addString((task.value())->name, counter);
    }

    for (QMap<int, TaskGroup *>::Iterator tg = trace->taskgroups->begin();
         tg != trace->taskgroups->end(); ++tg)
    {
        counter = addString((tg.value())->name, counter);
    }

    for (QMap<int, Function *>::Iterator fxn = trace->functions->begin();
         fxn != trace->functions->end(); ++fxn)
    {
        counter = addString((fxn.value())->name, counter);
    }

    for (QList<QString>::Iterator metric = trace->metrics->begin();
         metric != trace->metrics->end(); ++metric)
    {
        counter = addString((*metric), counter);
        counter = addString((*metric) + "_agg", counter);
    }
    for (QMap<QString, QString>::Iterator unit = trace->metric_units->begin();
         unit != trace->metric_units->end(); ++unit)
    {
        counter = addString(unit.value(), counter);
    }

    QList<QString> options = trace->options.getOptionNames();
    for (QList<QString>::Iterator opt = options.begin();
         opt != options.end(); ++opt)
    {
        counter = addString(*opt, counter);
        counter = addString(trace->options.getOptionValue(*opt), counter);
    }
}

int OTF2Exporter::addString(QString str, int counter)
{
    if (inverseStringMap.contains(str))
        return counter;

    counter++;
    OTF2_GlobalDefWriter_WriteString(global_def_writer, counter,
                                     str.toStdString().c_str());
    inverseStringMap.insert(str, counter);
    return counter;
}
