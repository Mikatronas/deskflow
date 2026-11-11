// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "deskflow/KeyTypes.h"

// Apply at the receiving peer; never replace the physical button identity.
inline KeyID keypadDigits(KeyID id)
{
  switch (id) {
  case kKeyKP_Insert:
    return kKeyKP_0;
  case kKeyKP_End:
    return kKeyKP_1;
  case kKeyKP_Down:
    return kKeyKP_2;
  case kKeyKP_PageDown:
    return kKeyKP_3;
  case kKeyKP_Left:
    return kKeyKP_4;
  case kKeyKP_Begin:
    return kKeyKP_5;
  case kKeyKP_Right:
    return kKeyKP_6;
  case kKeyKP_Home:
    return kKeyKP_7;
  case kKeyKP_Up:
    return kKeyKP_8;
  case kKeyKP_PageUp:
    return kKeyKP_9;
  case kKeyKP_Delete:
    return kKeyKP_Decimal;
  default:
    return id;
  }
}
