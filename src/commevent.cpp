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
#include "commevent.h"
#include "clusterevent.h"
#include <otf2/OTF2_AttributeList.h>
#include <otf2/OTF2_GeneralDefinitions.h>
#include <iostream>

CommEvent::CommEvent(unsigned long long _enter, unsigned long long _exit,
                     int _function, int _task, int _phase)
    : Event(_enter, _exit, _function, _task),
      metrics(new QMap<QString, MetricPair *>()),
      partition(NULL),
      comm_next(NULL),
      comm_prev(NULL),
      last_stride(NULL),
      next_stride(NULL),
      last_recvs(NULL),
      last_step(-1),
      stride_parents(new QSet<CommEvent *>()),
      stride_children(new QSet<CommEvent *>()),
      stride(-1),
      step(-1),
      phase(_phase)
{
}

CommEvent::~CommEvent()
{
    for (QMap<QString, MetricPair *>::Iterator itr = metrics->begin();
         itr != metrics->end(); ++itr)
    {
        delete itr.value();
    }
    delete metrics;

    if (last_recvs)
        delete last_recvs;
    if (stride_children)
        delete stride_children;
    if (stride_parents)
        delete stride_parents;
}



void CommEvent::addMetric(QString name, double event_value,
                          double aggregate_value)
{
    (*metrics)[name] = new MetricPair(event_value, aggregate_value);
}

void CommEvent::setMetric(QString name, double event_value,
                          double aggregate_value)
{
    MetricPair * mp = metrics->value(name);
    mp->event = event_value;
    mp->aggregate = aggregate_value;
}

bool CommEvent::hasMetric(QString name)
{
    return metrics->contains(name);
}

double CommEvent::getMetric(QString name, bool aggregate)
{
    if (aggregate)
        return ((*metrics)[name])->aggregate;

    return ((*metrics)[name])->event;
}

void CommEvent::calculate_differential_metric(QString metric_name,
                                              QString base_name)
{
    long long max_parent = getMetric(base_name, true);
    long long max_agg_parent = 0;
    if (comm_prev)
        max_agg_parent = (comm_prev->getMetric(base_name));

    addMetric(metric_name,
              std::max(0.,
                       getMetric(base_name)- max_parent),
              std::max(0.,
                       getMetric(base_name, true)- max_agg_parent));
}

void CommEvent::writeOTF2Leave(OTF2_EvtWriter * writer, QMap<QString, int> * attributeMap)
{
    // For coalesced steps, don't write attributes
    if (step < 0)
    {
        // Finally write enter event
        OTF2_EvtWriter_Leave(writer,
                             NULL,
                             exit,
                             function);
        return;
    }

    OTF2_AttributeList * attribute_list = OTF2_AttributeList_New();

    // Phase and Step
    OTF2_AttributeValue phase_value;
    phase_value.uint32 = phase;
    OTF2_AttributeList_AddAttribute(attribute_list,
                                    attributeMap->value("phase"),
                                    OTF2_TYPE_UINT64,
                                    phase_value);

    OTF2_AttributeValue step_value;
    step_value.uint32 = step;
    OTF2_AttributeList_AddAttribute(attribute_list,
                                    attributeMap->value("step"),
                                    OTF2_TYPE_UINT64,
                                    step_value);

    // Write metrics
    for (QMap<QString, int>::Iterator attr = attributeMap->begin();
         attr != attributeMap->end(); ++attr)
    {
        if (!hasMetric(attr.key()))
            continue;

        OTF2_AttributeValue attr_value;
        attr_value.uint64 = getMetric(attr.key());
        OTF2_AttributeList_AddAttribute(attribute_list,
                                        attributeMap->value(attr.key()),
                                        OTF2_TYPE_UINT64,
                                        attr_value);

        OTF2_AttributeValue agg_value;
        agg_value.uint64 = getMetric(attr.key(), true);
        OTF2_AttributeList_AddAttribute(attribute_list,
                                        attributeMap->value(attr.key() + "_agg"),
                                        OTF2_TYPE_UINT64,
                                        agg_value);
    }

    // Finally write enter event
    OTF2_EvtWriter_Leave(writer,
                         attribute_list,
                         exit,
                         function);
}
