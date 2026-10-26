#pragma once

// Private to the BindingEditorModel translation units (BindingEditorModel*.cpp):
// helpers more than one of them needs. Not part of the class API.

#include "input/BindingRelation.h"

namespace BindingEditorInternal
{
// A new binding can relate to several existing ones at once. effectiveBindings()
// comes out of a QHash, so "first match wins" would show a different notice on
// different runs. Rank the non-blocking tiers by how much the user needs to
// know: a context override changes what the button does, a redundant binding
// means their edit did nothing, and a shared gesture is merely informational.
inline int noticeSeverity(BindingRelation::Kind kind)
{
    switch (kind) {
    case BindingRelation::Kind::ContextOverride: return 3;
    case BindingRelation::Kind::Redundant:       return 2;
    case BindingRelation::Kind::SharedGesture:   return 1;
    case BindingRelation::Kind::ConversionRequired:     // owns its compatibility modal
    case BindingRelation::Kind::HardConflict:            // owns the modal instead
    case BindingRelation::Kind::None:            return 0;
    }
    return 0;
}
} // namespace BindingEditorInternal
