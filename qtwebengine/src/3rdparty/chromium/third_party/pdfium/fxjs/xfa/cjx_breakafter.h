// Copyright 2017 PDFium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef FXJS_XFA_CJX_BREAKAFTER_H_
#define FXJS_XFA_CJX_BREAKAFTER_H_

#include "fxjs/xfa/cjx_node.h"

class CXFA_BreakAfter;

class CJX_BreakAfter : public CJX_Node {
 public:
  explicit CJX_BreakAfter(CXFA_BreakAfter* node);
  ~CJX_BreakAfter() override;

  JS_PROP(leader);
  JS_PROP(startNew);
  JS_PROP(target);
  JS_PROP(targetType);
  JS_PROP(trailer);
  JS_PROP(use);
  JS_PROP(usehref);
};

#endif  // FXJS_XFA_CJX_BREAKAFTER_H_
