#pragma once

#include "input/BindingResolver.h"

#include <QVector>

// The code-owned default binding table, extracted from BindingResolver so the
// shipped defaults can be audited without a database or a device: the overlay's
// input contract (tests/tst_overlayinput.cpp) is asserted against exactly these
// rows, and BindingResolver::defaultBindings is the only production reader.
QVector<BindingResolver::Binding> gamehqDefaultBindings();
