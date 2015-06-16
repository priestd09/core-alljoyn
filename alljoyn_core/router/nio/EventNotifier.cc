/*
 * EventNotifier.cc
 *
 *  Created on: May 18, 2015
 *      Author: erongo
 */
/******************************************************************************
 * Copyright AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/

#include "EventNotifier.h"
#include "DispatcherBase.h"

#include <assert.h>

using namespace nio;

EventNotifier::EventNotifier(EventCallback cb) : callback(cb), dispatcher(nullptr)
{
}

EventNotifier::~EventNotifier()
{
}


void EventNotifier::ExecuteInternal()
{
    callback();
}

void EventNotifier::Signal()
{
    assert(dispatcher && "Must register before calling Signal!\n");
    auto func = [this] () {
                    Execute();
                };
    // Execute --> <lock> --> ExecuteInternal --> callback --> <unlock>
    dispatcher->Dispatch(func);
}

void EventNotifier::SetDispatcher(DispatcherBase* dispatcher)
{
    this->dispatcher = dispatcher;
}
