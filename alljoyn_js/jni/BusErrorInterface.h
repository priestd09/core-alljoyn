/*
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
 */
#ifndef _BUSERRORINTERFACE_H
#define _BUSERRORINTERFACE_H

#include "ScriptableObject.h"
#include <qcc/ManagedObj.h>

class _BusErrorInterface : public ScriptableObject {
  public:
    static std::map<qcc::String, int32_t>& Constants();
    _BusErrorInterface(Plugin& plugin);
    virtual ~_BusErrorInterface();

  private:
    static std::map<qcc::String, int32_t> constants;
    bool getName(NPVariant* result);
    bool getMessage(NPVariant* result);
    bool getCode(NPVariant* result);
};

typedef qcc::ManagedObj<_BusErrorInterface> BusErrorInterface;

#endif // _BUSERRORINTERFACE_H
